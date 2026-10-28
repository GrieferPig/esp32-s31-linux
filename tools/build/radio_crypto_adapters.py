#!/usr/bin/env python3
"""Build checked crypto adapters from the pinned SDK without editing its tree.

The upstream arithmetic stays in its original translation unit. Checked leases
and bounded completion waits replace void/fatal hardware boundaries. Every
transformation is count checked and source identities are pinned explicitly.
"""
import argparse
import hashlib
import json
import shlex
import subprocess
import re
from pathlib import Path

SOURCES = {
    'ecc': ('components/mbedtls/port/ecc/esp_ecc.c', 'd8171f9e22d15cad7341e6289d12e9c877328747cf25644c9e90f11f5f41f8ce'),
    'mpi': ('components/mbedtls/port/bignum/esp_bignum.c', 'b52e1a17d8d480c573f7b0ca15882244d14835b1163d078abaa48705c5e965c7'),
    'ecdsa': ('components/mbedtls/port/psa_driver/esp_ecdsa/psa_crypto_driver_esp_ecdsa.c', '9d852b641f699e41db6a81036beed3e0d5a8ca72496e8ab4cd46065e5bef4b83'),
    'hmac': ('components/esp_security/src/esp_hmac.c', 'a55876fce7cf8dbcd79a77960eb69968668f10431198205747eda6e8d9354800'),
    'tinycrypt': ('components/bt/common/tinycrypt/port/esp_tinycrypt_port.c', '6d10a367194ea49a9c4e3181ea3c882ae1cd83413189094c15edc63b62d199a5'),
}

HAL_SOURCES = {
    'hmac': ('components/esp_hal_security/hmac_hal.c', '71398f54da9065dd8348b9aea8662e38f66b05e44dc1d027c784c9d4d8d2b36d'),
    'ecdsa': ('components/esp_hal_security/ecdsa_hal.c', '91599a311958ea8bd32fb86c2f2d0e5f36b71c330a7a34b67ebdefd52b26126f'),
}


def replace(text, old, new, count):
    if text.count(old) != count:
        raise ValueError(f'crypto adapter source mismatch: {old!r}')
    return text.replace(old, new)


def spans(text):
    # Preserve byte offsets while excluding braces in comments and literals.
    stripped = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                      lambda m: ' ' * len(m[0]), text, flags=re.S)
    for m in re.finditer(r'^\w[^;{}]*?\b(\w+)\s*\([^;{}]*\)\s*\{', stripped, re.M):
        start, depth, i = m.end() - 1, 1, m.end()
        while depth:
            depth += (stripped[i] == '{') - (stripped[i] == '}')
            i += 1
        yield m[1], m.start(), start, i


def checked_hal(name, text):
    """Keep the SDK's parameter transfers, but propagate hardware timeouts."""
    if name == 'hmac':
        names = {'hmac_hal_start', 'hmac_hal_configure',
                 'hmac_hal_write_one_block_512', 'hmac_hal_write_block_512',
                 'hmac_hal_next_block_padding', 'hmac_hal_next_block_normal',
                 'hmac_hal_read_result_256'}
        result = ['#include "hal/hmac_ll.h"\n#include "hal/key_mgr_hal.h"\n'
                  '#include "hal/key_mgr_ll.h"\n']
        for fn, start, brace, end in spans(text):
            if fn not in names:
                continue
            body = text[brace:end]
            waits = body.count('hmac_ll_wait_idle();')
            body = replace(body, 'hmac_ll_wait_idle();',
                           'if (!S31_SDK_CRYPTO_WAIT_READY(!REG_READ(HMAC_QUERY_BUSY_REG))) return -1;', waits)
            if fn == 'hmac_hal_configure':
                body = replace(body, 'HAL_ASSERT(false && "Key manager is not supported");', 'return 1;', 1)
            if fn != 'hmac_hal_configure':
                body = body[:-1] + '    return 0;\n}'
            signature = re.sub(r'^\w+\s+' + fn, 'static int s31_sdk_' + fn,
                               text[start:brace])
            result.append(signature + body + '\n')
            names.remove(fn)
        if names:
            raise ValueError(f'missing HMAC HAL functions: {names}')
        return ''.join(result)
    if name == 'ecdsa':
        for fn, start, brace, end in spans(text):
            if fn != 'ecdsa_hal_verify_signature':
                continue
            body = text[brace:end]
            for bits in (192, 256, 384):
                body = replace(body, f'ECDSA_HAL_P{bits}_COMPONENT_LEN', str(bits // 8), 1)
            body = replace(body, 'HAL_ASSERT(false && "Incorrect length");', 'return -3;', 1)
            body = replace(body, 'HAL_ASSERT(false && "Incorrect ECDSA state");', 'return -2;', 1)
            # Public verification supplies Qx/Qy explicitly. It neither needs
            # nor may refresh/select a private eFuse/KM key. This also avoids
            # the SDK's unbounded eFuse-ROM read on a public-only operation.
            body = replace(body, 'configure_ecdsa_periph(conf);',
                           'ecdsa_ll_set_mode(conf->mode);\n'
                           '    ecdsa_ll_set_curve(conf->curve);\n'
                           '    ecdsa_ll_set_z_mode(conf->sha_mode);', 1)
            for state in ('LOAD', 'IDLE'):
                body = replace(body, f'while (ecdsa_ll_get_state() != ECDSA_STATE_{state}) {{\n        ;\n    }}',
                               f'if (!S31_SDK_CRYPTO_WAIT_READY(ecdsa_ll_get_state() == ECDSA_STATE_{state})) return -2;', 1)
            signature = text[start:brace].replace('int ecdsa_hal_verify_signature',
                                                 'static int s31_sdk_ecdsa_verify_signature')
            return ('#include "hal/ecdsa_hal.h"\n#include "hal/ecdsa_ll.h"\n' +
                    signature + body + '\n')
        raise ValueError('missing ECDSA HAL verify')
    return ''


MPI_HELPERS = '''
#include "hal/mpi_ll.h"
static int s31_sdk_mpi_enable(void)
{
    if (!S31_SDK_CRYPTO_WAIT_READY(!mpi_ll_check_memory_init_complete())) return -1;
    mpi_ll_disable_interrupt();
    return 0;
}
static int s31_sdk_mpi_read(uint32_t *p, size_t n, size_t words)
{
    if (!S31_SDK_CRYPTO_WAIT_READY(!mpi_ll_get_int_status())) return -1;
    mpi_ll_clear_interrupt();
    mpi_ll_read_from_mem_block(p, n, words);
    return 0;
}
'''


def transform(name, text, hal=''):
    functions = list(spans(text))
    edits = []
    for fn, start, brace, end in functions:
        body = text[brace:end]
        if name == 'ecc':
            if fn in ('esp_ecc_acquire_hardware', 'esp_ecc_release_hardware'):
                edits.append((start, end, ''))
                continue
            if 'esp_ecc_acquire_hardware();' not in body:
                continue
            failure = '-1' if fn == 'esp_ecc_point_multiply' else '0'
            body = replace(body, 'esp_ecc_acquire_hardware();',
                           'struct s31_sdk_crypto_lease lease = {0};\n'
                           '    if (s31_sdk_crypto_begin(&lease, S31_CRYPTO_ECC))\n'
                           f'        return {failure};', 1)
            clear = 'memset(result, 0, sizeof(*result)); ' if failure == '-1' else ''
            body = replace(body, 'esp_ecc_release_hardware();',
                           f'if (s31_sdk_crypto_end(&lease)) {{ {clear}return {failure}; }}', 1)
            body = replace(body, 'while (!ecc_hal_is_calc_finished()) {\n        ;\n    }',
                           'if (!S31_SDK_CRYPTO_WAIT_READY(ecc_hal_is_calc_finished())) {\n'
                           '        s31_sdk_crypto_fail(&lease);\n'
                           f'        {clear}return {failure};\n    }}', 1)
        elif name == 'tinycrypt':
            if fn in ('esp_tinycrypt_acquire_ecc_hardware', 'esp_tinycrypt_release_ecc_hardware'):
                edits.append((start, end, ''))
                continue
            if 'esp_tinycrypt_acquire_ecc_hardware();' not in body:
                continue
            width = 'length' if fn == 'esp_tinycrypt_verify_ecc_point' else 'num_bytes'
            body = replace(body, '#if SOC_ECC_SUPPORTED',
                           '#if SOC_ECC_SUPPORTED\n'
                           f'    if ({width} != 24 && {width} != 32 && {width} != 48) return -1;', 1)
            body = replace(body, 'esp_tinycrypt_acquire_ecc_hardware();',
                           'struct s31_sdk_crypto_lease lease = {0};\n'
                           '    if (s31_sdk_crypto_begin(&lease, S31_CRYPTO_ECC)) return -1;', 1)
            body = replace(body, 'esp_tinycrypt_release_ecc_hardware();',
                           'if (s31_sdk_crypto_end(&lease)) return -1;', 1)
            body = replace(body, 'while (!ecc_hal_is_calc_finished());',
                           'if (!S31_SDK_CRYPTO_WAIT_READY(ecc_hal_is_calc_finished()))\n'
                           '        return s31_sdk_crypto_fail(&lease);', 1)
        elif name == 'mpi':
            if fn == 'esp_mpi_complete_isr':
                body = replace(body, 'BaseType_t higher_woken;', 'BaseType_t higher_woken = pdFALSE;', 1)
                edits.append((brace, end, body))
                continue
            if fn == 'esp_mpi_isr_initialise':
                body = replace(body, 'mpi_hal_interrupt_enable(true);', 'mpi_hal_interrupt_enable(false);', 1)
                body = replace(body, 'abort();',
                               'vSemaphoreDelete(op_complete_sem);\n'
                               '            op_complete_sem = NULL;\n            return ret;', 1)
                body = replace(body, '    return ESP_OK;',
                               '    /* No previous IRQ may satisfy a new operation. */\n'
                               '    xSemaphoreTake(op_complete_sem, 0);\n'
                               '    mpi_hal_interrupt_enable(true);\n    return ESP_OK;', 1)
                # Allocation failure is a recoverable API error, not fatal.
                body = replace(body, '            // This should be treated as fatal error as this API would mostly\n'
                               '            // be invoked within mbedTLS interface. There is no way for the system\n'
                               '            // to proceed if the MPI interrupt allocation fails here.\n', '', 1)
                edits.append((brace, end, body))
                continue
            if fn == 'esp_mpi_wait_intr':
                body = replace(body, '    if (!xSemaphoreTake(op_complete_sem, 2000 / portTICK_PERIOD_MS)) {',
                               '    int ret = xSemaphoreTake(op_complete_sem, 2000 / portTICK_PERIOD_MS) ? 0 : -1;\n'
                               '    if (ret) {', 1)
                body = replace(body, '        return -1;', '', 1)
                body = replace(body, '    return 0;', '    return ret;', 1)
                edits.append((brace, end, body))
                continue
            if 'esp_mpi_enable_hardware_hw_op();' not in body:
                continue
            body = body[:1] + '\n    struct s31_sdk_crypto_lease lease = {0};\n' + body[1:]
            body = replace(body, 'esp_mpi_enable_hardware_hw_op();',
                           'if (s31_sdk_crypto_begin(&lease, S31_CRYPTO_MPI)) {\n'
                           '        ret = S31_SDK_CRYPTO_FAILURE; goto cleanup;\n'
                           '    }\n    if (s31_sdk_mpi_enable()) {\n'
                           '        ret = s31_sdk_crypto_fail(&lease); goto cleanup;\n    }', 1)
            reads = body.count('mpi_hal_read_result_hw_op(')
            body, n = re.subn(r'mpi_hal_read_result_hw_op\(([^;]+)\);',
                             r'if (s31_sdk_mpi_read(\1)) {\n'
                             r'        ret = s31_sdk_crypto_fail(&lease); goto cleanup;\n    }', body)
            expected_reads = 0 if fn == 'mpi_montgomery_exp_calc' else 1
            if reads != expected_reads or n != reads:
                raise ValueError(f'{fn}: unexpected MPI result reads')
            if 'ret = esp_mpi_wait_intr();' in body:
                body = replace(body, 'ret = esp_mpi_wait_intr();',
                               'ret = esp_mpi_wait_intr();\n'
                               '    if (ret) ret = s31_sdk_crypto_fail(&lease);', 1)
            if 'assert(z_words - mpi_words(Z) <= (size_t)1);' in body:
                body = replace(body, 'assert(z_words - mpi_words(Z) <= (size_t)1);',
                               'if (z_words - mpi_words(Z) > (size_t)1)\n'
                               '        ret = s31_sdk_crypto_fail(&lease);', 1)
            count = body.count('esp_mpi_disable_hardware_hw_op();')
            body = replace(body, 'esp_mpi_disable_hardware_hw_op();',
                           'if (s31_sdk_crypto_end(&lease)) ret = S31_SDK_CRYPTO_FAILURE;', count)
            # Error exits must release a lease even if an intermediate MPI
            # allocation failed. Idempotent cleanup also covers pre-admission.
            body = replace(body, 'cleanup:',
                           'cleanup:\n    if (s31_sdk_crypto_end(&lease)) ret = S31_SDK_CRYPTO_FAILURE;', 1)
        elif name == 'ecdsa':
            if fn in ('esp_ecdsa_acquire_hardware', 'esp_ecdsa_release_hardware'):
                edits.append((start, end, ''))
                continue
            if 'esp_ecdsa_acquire_hardware();' not in body:
                continue
            body = body[:1] + '\n    struct s31_sdk_crypto_lease lease = {0};\n' + body[1:]
            body = replace(body, 'esp_ecdsa_acquire_hardware();',
                           'if (s31_sdk_crypto_begin(&lease, S31_CRYPTO_ECDSA))\n'
                           '        return PSA_ERROR_HARDWARE_FAILURE;', 1)
            count = body.count('esp_ecdsa_release_hardware();')
            body = replace(body, 'esp_ecdsa_release_hardware();',
                           'if (s31_sdk_crypto_end(&lease)) return PSA_ERROR_HARDWARE_FAILURE;', count)
            if fn == 'esp_ecdsa_transparent_verify_hash_complete':
                body = replace(body, 'ecdsa_hal_verify_signature(', 's31_sdk_ecdsa_verify_signature(', 1)
                body = replace(body, '    if (s31_sdk_crypto_end(&lease))',
                               '    if (ret == -2) {\n        s31_sdk_crypto_fail(&lease);\n'
                               '        return PSA_ERROR_HARDWARE_FAILURE;\n    }\n'
                               '    if (s31_sdk_crypto_end(&lease))', 1)
                body = replace(body, '    if (ret != 0) {',
                               '    if (ret == -3) return PSA_ERROR_INVALID_ARGUMENT;\n'
                               '    if (ret != 0) {', 1)
        elif name == 'hmac':
            if fn != 'esp_hmac_calculate' or 'esp_crypto_hmac_lock_acquire();' not in body:
                continue
            body = body[:1] + '\n    struct s31_sdk_crypto_lease lease = {0};\n' + body[1:]
            body = replace(body, 'esp_crypto_hmac_lock_acquire();',
                           'if (s31_sdk_crypto_begin(&lease, S31_CRYPTO_HMAC)) return ESP_FAIL;', 1)
            body = re.sub(r'esp_crypto_\w+_enable_periph_clk\((?:true|false)\);', '', body)
            body = replace(body, 'esp_crypto_hmac_lock_release();',
                           'if (s31_sdk_crypto_end(&lease)) { memset(hmac, 0, 32); return ESP_FAIL; }', 2)
            body = replace(body, 'uint32_t conf_error = hmac_hal_configure(HMAC_OUTPUT_USER, key_id);',
                           'int conf_error = s31_sdk_hmac_hal_configure(HMAC_OUTPUT_USER, key_id);\n'
                           '    if (conf_error < 0) goto hardware_failed;', 1)
            body, n = re.subn(r'(?m)^(\s*)(hmac_hal_(?:start|write_one_block_512|write_block_512|next_block_padding|next_block_normal|read_result_256))\(([^;]*)\);',
                             r'\1if (s31_sdk_\2(\3)) goto hardware_failed;', body)
            if n != 10:
                raise ValueError(f'HMAC: unexpected HAL calls {n}')
            body = replace(body, 'message_len + 1 + SHA256_PAD_SZ <= SHA256_BLOCK_SZ',
                           'message_len <= SHA256_BLOCK_SZ - 1 - SHA256_PAD_SZ', 1)
            body = replace(body, 'key_id >= HMAC_KEY_MAX', '(unsigned int)key_id >= HMAC_KEY_MAX', 1)
            body = body[:-1] + ('hardware_failed:\n    s31_sdk_crypto_fail(&lease);\n'
                               '    memset(hmac, 0, 32);\n    return ESP_FAIL;\n}')
        else:
            raise ValueError(name)
        edits.append((brace, end, body))
    expected = {'ecc': 4, 'mpi': 8, 'ecdsa': 5, 'hmac': 1, 'tinycrypt': 4}[name]
    if len(edits) != expected:
        raise ValueError(f'{name}: expected {expected} transformed functions, got {len(edits)}')
    for start, end, body in reversed(edits):
        text = text[:start] + body + text[end:]
    return '#include "s31_sdk_crypto.h"\n#include <stddef.h>\n' + hal + (MPI_HELPERS if name == 'mpi' else '') + text


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--idf', type=Path)
    p.add_argument('--output', type=Path)
    p.add_argument('--build', type=Path)
    p.add_argument('--audit', type=Path)
    p.add_argument('--objdump')
    a = p.parse_args()
    if a.audit:
        if not a.objdump:
            p.error('--audit requires --objdump')
        relocs = subprocess.check_output([a.objdump, '-r', a.audit], text=True)
        unsafe = [line for line in relocs.splitlines() if 'R_RISCV' in line and
                  re.search(r'(?:__wrap_)?esp_crypto_\w+_(?:lock_acquire|lock_release|enable_periph_clk)\b|'
                            r'\b(?:mpi_hal_(?:enable_hardware_hw_op|read_result_hw_op|wait_op_complete)|'
                            r'hmac_hal_\w+|ecdsa_hal_(?:verify_signature|gen_signature|export_pubkey))\b', line)]
        if unsafe:
            raise ValueError('unchecked SDK crypto call sites: ' + '; '.join(unsafe))
        print('radio-crypto: surviving SDK callers use checked leases and bounded completion waits')
        return
    if not all((a.idf, a.build, a.output)):
        p.error('--idf, --build and --output are required for generation')
    a.output.mkdir(parents=True, exist_ok=True)
    commands = json.loads((a.build / "compile_commands.json").read_text())
    for name, (source, digest) in SOURCES.items():
        data = (a.idf / source).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f'{source}: incompatible SDK revision')
        output = a.output / f's31_sdk_{name}.c'
        hal = ''
        if name in HAL_SOURCES:
            hal_source, hal_digest = HAL_SOURCES[name]
            hal_data = (a.idf / hal_source).read_bytes()
            if hashlib.sha256(hal_data).hexdigest() != hal_digest:
                raise ValueError(f'{hal_source}: incompatible SDK revision')
            hal = checked_hal(name, hal_data.decode())
        result = transform(name, data.decode(), hal)
        if not output.exists() or output.read_text() != result:
            output.write_text(result)
        entries = [entry for entry in commands
                   if Path(entry['file']).resolve() == (a.idf / source).resolve()]
        if len(entries) != 1:
            raise ValueError(f'{source}: missing/ambiguous SDK compile command')
        # Use this component's own SDK headers and configuration definitions,
        # retaining the radio compiler/ABI/optimization and depfile settings.
        flags = [arg for arg in shlex.split(entries[0]['command'])
                 if arg.startswith(('-I', '-D'))]
        response = a.output / f's31_sdk_{name}.rsp'
        contents = shlex.join(flags) + '\n'
        if not response.exists() or response.read_text() != contents:
            response.write_text(contents)


if __name__ == '__main__':
    main()

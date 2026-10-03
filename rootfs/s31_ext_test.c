// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "esp_simd.h"

extern uint32_t s31_fpu_add(uint32_t a, uint32_t b);
extern void s31_fpu_all(uint32_t *out, const uint32_t *input);
extern uint32_t s31_fpu_hold_yield(uint32_t value);
extern uint32_t s31_fpu_hold_signal(uint32_t value, pid_t pid);
extern uint32_t s31_hwloop_setupi(void);
extern uint32_t s31_hwloop_split_imm(void);
extern uint32_t s31_hwloop_split_reg(void);
extern void s31_hwloop_complete_yield(volatile uint32_t *counter);
extern void s31_hwloop_yield(volatile uint32_t *counter);
extern void s31_hwloop_signal(volatile uint32_t *counter, pid_t pid);
extern void s31_pie_add_u32(const uint32_t *a, const uint32_t *b,
			    uint32_t *out);
extern void s31_pie_cmp_eq_u8(const uint8_t *a, const uint8_t *b,
			      uint8_t *out);
extern void s31_pie_hold_yield(const uint32_t *in, uint32_t *out);
extern void s31_pie_hold_signal(const uint32_t *in, uint32_t *out, pid_t pid);
extern void s31_pie_run_one(unsigned int index, void *scratch);
extern void s31_bitmanip_all(uint32_t *out);

#define PIE_CASE(opcode, name) name,
static const char *const pie_case_names[] = {
#include <s31_pie_cases.inc>
};
#undef PIE_CASE



#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static volatile sig_atomic_t signal_count;
static volatile uint32_t signal_hwloop_count;

static void clobber_handler(int signo)
{
	static const uint32_t a[4] __attribute__((aligned(16))) = {
		0xdead0001, 0xdead0002, 0xdead0003, 0xdead0004
	};
	static const uint32_t b[4] __attribute__((aligned(16))) = { 1, 2, 3, 4 };
	uint32_t out[4] __attribute__((aligned(16)));

	(void)signo;
	(void)s31_fpu_add(0x3f800000, 0x40000000);
	s31_pie_add_u32(a, b, out);
	s31_hwloop_yield(&signal_hwloop_count);
	signal_count++;
}

static int signal_tests(void)
{
	static const uint32_t in[4] __attribute__((aligned(16))) = {
		0x51000001, 0x51000002, 0x51000003, 0x51000004
	};
	uint32_t out[4] __attribute__((aligned(16))) = { 0 };
	volatile uint32_t count = 0;
	struct sigaction sa = { .sa_handler = clobber_handler };
	pid_t pid = getpid();
	unsigned int lane;

	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGUSR1, &sa, NULL)) {
		perror("sigaction");
		return -1;
	}

	if (s31_fpu_hold_signal(0x3f400000, pid) != 0x3f400000) {
		fprintf(stderr, "FPU signal context corrupt\n");
		return -1;
	}
	s31_pie_hold_signal(in, out, pid);
	for (lane = 0; lane < 4; lane++) {
		if (out[lane] != in[lane]) {
			fprintf(stderr, "PIE signal lane %u corrupt: %08x\n",
				lane, out[lane]);
			return -1;
		}
	}
	s31_hwloop_signal(&count, pid);
	if (count != 8) {
		fprintf(stderr, "HWLoop signal count corrupt: %u\n",
			(unsigned int)count);
		return -1;
	}
	if (signal_count != 10 || signal_hwloop_count != 640) {
		fprintf(stderr, "signal handler counts corrupt: %d/%u\n",
			(int)signal_count, (unsigned int)signal_hwloop_count);
		return -1;
	}
	return 0;
}

static int basic_tests(void)
{
	static const uint32_t a[4] __attribute__((aligned(16))) = { 1, 2, 3, 4 };
	static const uint32_t b[4] __attribute__((aligned(16))) = { 5, 7, 11, 13 };
	uint32_t out[4] __attribute__((aligned(16))) = { 0 };
	static const uint32_t expected[4] = { 6, 9, 14, 17 };
	uint32_t f;
	int i;

	/* 1.5f + 2.25f = 3.75f; values are passed as raw IEEE-754 bits. */
	f = s31_fpu_add(0x3fc00000, 0x40100000);
	if (f != 0x40700000) {
		fprintf(stderr, "FPU add failed: %08x\n", f);
		return -1;
	}

	s31_pie_add_u32(a, b, out);
	for (i = 0; i < 4; i++) {
		if (out[i] != expected[i]) {
			fprintf(stderr, "PIE add lane %d failed: %08x\n", i,
				out[i]);
			return -1;
		}
	}

	{
		static const uint8_t lhs[16] __attribute__((aligned(16))) = {
			0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
		};
		static const uint8_t rhs[16] __attribute__((aligned(16))) = {
			0, 9, 2, 8, 4, 7, 6, 6, 8, 5, 10, 4, 12, 3, 14, 2
		};
		uint8_t cmp[16] __attribute__((aligned(16))) = { 0 };

		s31_pie_cmp_eq_u8(lhs, rhs, cmp);
		printf("XespV vcmp.eq.u8 lanes:");
		for (i = 0; i < 16; i++)
			printf(" %02x", cmp[i]);
		putchar('\n');
	}

	return 0;
}

static int all_fpu_instruction_tests(void)
{
	static const uint32_t input = 0x41200000; /* 10.0 */
	static const uint32_t expected[] = {
		0x41200000, /* flw + fsw */
		0x40600000, /* fmadd.s:  1.5 * 2.0 + 0.5 =  3.5 */
		0x40200000, /* fmsub.s:  1.5 * 2.0 - 0.5 =  2.5 */
		0xc0200000, /* fnmsub.s: -(1.5 * 2.0) + 0.5 = -2.5 */
		0xc0600000, /* fnmadd.s: -(1.5 * 2.0) - 0.5 = -3.5 */
		0x40600000, /* fadd.s */
		0xbf000000, /* fsub.s */
		0x40400000, /* fmul.s */
		0x3f400000, /* fdiv.s */
		0x40000000, /* fsqrt.s */
		0xbfc00000, /* fsgnj.s */
		0x3fc00000, /* fsgnjn.s */
		0xbfc00000, /* fsgnjx.s */
		0xc0000000, /* fmin.s */
		0x3fc00000, /* fmax.s */
		2,          /* fcvt.w.s */
		2,          /* fcvt.wu.s */
		0x3fc00000, /* fmv.x.w */
		1,          /* feq.s */
		1,          /* flt.s */
		0,          /* fle.s */
		2,          /* fclass.s: negative normal */
		0xc0400000, /* fcvt.s.w */
		0x40400000, /* fcvt.s.wu */
		0x3f800000, /* fmv.w.x */
		1,          /* frrm after fsrm */
		0x1f,       /* frflags after fsflags */
	};
	uint32_t out[ARRAY_SIZE(expected)];
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(out); i++)
		out[i] = 0xdeadbeef;
	s31_fpu_all(out, &input);
	for (i = 0; i < ARRAY_SIZE(out); i++) {
		if (out[i] != expected[i]) {
			fprintf(stderr,
				"RV32F instruction %u failed: got %08x expected %08x\n",
				i, out[i], expected[i]);
			return -1;
		}
	}
	printf("RV32F: all %u instruction classes PASS\n",
	       (unsigned int)ARRAY_SIZE(expected));
	return 0;
}

static int all_hwloop_instruction_tests(void)
{
	volatile uint32_t setup_count = 0;

	if (s31_hwloop_setupi() != 5) {
		fprintf(stderr, "esp.lp.setupi failed\n");
		return -1;
	}
	if (s31_hwloop_split_imm() != 7) {
		fprintf(stderr, "esp.lp.starti/endi/counti failed\n");
		return -1;
	}
	if (s31_hwloop_split_reg() != 9) {
		fprintf(stderr, "esp.lp.starti/endi/count failed\n");
		return -1;
	}
	s31_hwloop_yield(&setup_count);
	if (setup_count != 64) {
		fprintf(stderr, "esp.lp.setup failed: %u\n",
			(unsigned int)setup_count);
		return -1;
	}
	puts("Xesploop: all 6 instruction forms PASS");
	return 0;
}

static int completed_hwloop_worker(unsigned int id)
{
	cpu_set_t mask;
	unsigned int iteration;

	CPU_ZERO(&mask);
	CPU_SET(0, &mask);
	if (sched_setaffinity(0, sizeof(mask), &mask)) {
		perror("CPU0 sched_setaffinity");
		return 1;
	}

	for (iteration = 0; iteration < 200; iteration++) {
		volatile uint32_t count = 0;

		s31_hwloop_complete_yield(&count);
		if (count != 64) {
			fprintf(stderr,
				"CPU0 worker %u: completed HWLoop count %u at %u\n",
				id, (unsigned int)count, iteration);
			return 1;
		}
	}
	return 0;
}

static int completed_hwloop_switch_tests(void)
{
	pid_t children[2];
	int status;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(children); i++) {
		children[i] = fork();
		if (children[i] < 0) {
			perror("CPU0 HWLoop fork");
			return -1;
		}
		if (!children[i])
			_exit(completed_hwloop_worker(i + 1));
	}

	for (i = 0; i < ARRAY_SIZE(children); i++) {
		if (waitpid(children[i], &status, 0) < 0) {
			perror("CPU0 HWLoop waitpid");
			return -1;
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status)) {
			fprintf(stderr, "CPU0 completed HWLoop worker %u failed, status=%04x\n",
				i, status);
			return -1;
		}
	}
	puts("Xesploop: completed-loop CPU0 context switches PASS");
	return 0;
}

static uint64_t carryless_product(uint32_t a, uint32_t b)
{
	uint64_t product = 0;
	unsigned int bit;

	for (bit = 0; bit < 32; bit++)
		if (b & (1U << bit))
			product ^= (uint64_t)a << bit;
	return product;
}

static int bitmanip_tests(void)
{
	const uint32_t a = 0x12345678;
	const uint32_t b = 0x0f0f0f0f;
	const uint64_t product = carryless_product(a, b);
	uint32_t out[6];

	s31_bitmanip_all(out);
	if (out[0] != (a << 1) + b ||
	    out[1] != (a & ~b) ||
	    out[2] != (a | (1U << 5)) ||
	    out[3] != (uint32_t)product ||
	    out[4] != (uint32_t)(product >> 32) ||
	    out[5] != (uint32_t)(product >> 31)) {
		fprintf(stderr,
			"bitmanip failed: %08x %08x %08x %08x %08x %08x\n",
			out[0], out[1], out[2], out[3], out[4], out[5]);
		return -1;
	}
	puts("Zba/Zbb/Zbc/Zbs representative instructions PASS");
	return 0;
}

static int all_pie_instruction_tests(void)
{
	static unsigned char scratch[4096] __attribute__((aligned(4096)));
	unsigned int failures = 0;
	unsigned int expected_unsupported = 0;
	unsigned int index;

	for (index = 0; index < ARRAY_SIZE(pie_case_names); index++) {
		pid_t child = fork();
		int status;

		if (child < 0) {
			perror("PIE fork");
			return -1;
		}
		if (!child) {
			alarm(2);
			s31_pie_run_one(index, scratch);
			_exit(0);
		}
		if (waitpid(child, &status, 0) < 0) {
			perror("PIE waitpid");
			return -1;
		}
		if (WIFSIGNALED(status) && WTERMSIG(status) == SIGILL &&
		    index == 270) {
			fprintf(stderr,
				"Xespv[%u] expected SIGILL on S31 rev0.0: %s\n",
				index, pie_case_names[index]);
			expected_unsupported++;
		} else if (!WIFEXITED(status) || WEXITSTATUS(status)) {
			if (WIFSIGNALED(status))
				fprintf(stderr,
					"Xespv[%u] signal %d: %s\n", index,
					WTERMSIG(status), pie_case_names[index]);
			else
				fprintf(stderr, "Xespv[%u] status %04x: %s\n",
					index, status, pie_case_names[index]);
			failures++;
		}
		if ((index & 31) == 31)
			printf("Xespv: %u/%u instruction forms passed\n",
			       index + 1,
			       (unsigned int)ARRAY_SIZE(pie_case_names));
	}
	if (failures) {
		fprintf(stderr, "Xespv: %u/%u instruction forms FAILED\n",
			failures, (unsigned int)ARRAY_SIZE(pie_case_names));
		return -1;
	}
	printf("Xespv: %u/%u instruction forms PASS, %u expected silicon exception\n",
	       (unsigned int)ARRAY_SIZE(pie_case_names) - expected_unsupported,
	       (unsigned int)ARRAY_SIZE(pie_case_names), expected_unsupported);
	return 0;
}

static int context_worker(unsigned int id)
{
	uint32_t in[4] __attribute__((aligned(16)));
	uint32_t out[4] __attribute__((aligned(16)));
	uint32_t fpat = 0x3f000000 + (id << 16);
	unsigned int iteration, lane;

	for (lane = 0; lane < 4; lane++)
		in[lane] = 0x31000000 ^ (id << 20) ^ (lane << 12);

	for (iteration = 0; iteration < 200; iteration++) {
		volatile uint32_t count = 0;

		if (s31_fpu_hold_yield(fpat) != fpat) {
			fprintf(stderr, "worker %u: FPU context corrupt at %u\n",
				id, iteration);
			return 1;
		}

		s31_hwloop_yield(&count);
		if (count != 64) {
			fprintf(stderr,
				"worker %u: HWLoop count %u at iteration %u\n",
				id, (unsigned int)count, iteration);
			return 1;
		}
		count = 0;
		s31_hwloop_complete_yield(&count);
		if (count != 64) {
			fprintf(stderr,
				"worker %u: completed HWLoop count %u at iteration %u\n",
				id, (unsigned int)count, iteration);
			return 1;
		}

		for (lane = 0; lane < 4; lane++)
			out[lane] = 0;
		s31_pie_hold_yield(in, out);
		for (lane = 0; lane < 4; lane++) {
			if (out[lane] != in[lane]) {
				fprintf(stderr,
					"worker %u: PIE q0 lane %u corrupt at %u\n",
					id, lane, iteration);
				return 1;
			}
		}
	}

	return 0;
}

int main(void)
{
	pid_t children[4];
	int status;
	int instruction_failures;
	unsigned int i;

	if (esp_simd_init()) {
		perror("esp_simd_init");
		return 1;
	}
	printf("XespV test affinity: CPU%d\n", esp_simd_cpu());

	if (basic_tests())
		return 1;
	if (all_fpu_instruction_tests())
		return 1;
	if (all_hwloop_instruction_tests())
		return 1;
	if (completed_hwloop_switch_tests())
		return 1;
	if (bitmanip_tests())
		return 1;
	instruction_failures = all_pie_instruction_tests();
	if (signal_tests())
		return 1;

	for (i = 0; i < 4; i++) {
		children[i] = fork();
		if (children[i] < 0) {
			perror("fork");
			return 1;
		}
		if (!children[i])
			_exit(context_worker(i + 1));
	}

	for (i = 0; i < 4; i++) {
		if (waitpid(children[i], &status, 0) < 0) {
			perror("waitpid");
			return 1;
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status)) {
			fprintf(stderr, "worker %u failed, status=%04x\n", i,
				status);
			return 1;
		}
	}

	if (instruction_failures) {
		puts("S31 FPU/HWLoop/PIE context tests: PASS; instruction failures listed above");
		return 1;
	}

	puts("S31 FPU/HWLoop/PIE instruction and context tests: PASS");
	return 0;
}

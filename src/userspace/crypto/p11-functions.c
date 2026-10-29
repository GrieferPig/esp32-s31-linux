/* SPDX-License-Identifier: Apache-2.0 */
#include "p11.h"

CK_RV C_InitToken(CK_SLOT_ID slotID, CK_UTF8CHAR_PTR pPin, CK_ULONG ulPinLen, CK_UTF8CHAR_PTR pLabel)
{
	CK_RV rv = p11_global_begin(); if (rv) return rv;
	(void)slotID;
	(void)pPin;
	(void)ulPinLen;
	(void)pLabel;
	return p11_global_end(slotID != 1 ? CKR_SLOT_ID_INVALID : CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_InitPIN(CK_SESSION_HANDLE hSession, CK_UTF8CHAR_PTR pPin, CK_ULONG ulPinLen)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pPin;
	(void)ulPinLen;
	return p11_session_end(s, CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_SetPIN(CK_SESSION_HANDLE hSession, CK_UTF8CHAR_PTR pOldPin, CK_ULONG ulOldLen, CK_UTF8CHAR_PTR pNewPin, CK_ULONG ulNewLen)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pOldPin;
	(void)ulOldLen;
	(void)pNewPin;
	(void)ulNewLen;
	return p11_session_end(s, CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_GetOperationState(CK_SESSION_HANDLE hSession, CK_BYTE_PTR pOperationState, CK_ULONG_PTR pulOperationStateLen)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pOperationState;
	(void)pulOperationStateLen;
	return p11_session_end(s, (s->sign.active || s->verify.active || s->encrypt.active || s->decrypt.active || s->digest_active) ? CKR_STATE_UNSAVEABLE : CKR_OPERATION_NOT_INITIALIZED);
}

CK_RV C_SetOperationState(CK_SESSION_HANDLE hSession, CK_BYTE_PTR pOperationState, CK_ULONG ulOperationStateLen, CK_OBJECT_HANDLE hEncryptionKey, CK_OBJECT_HANDLE hAuthenticationKey)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pOperationState;
	(void)ulOperationStateLen;
	(void)hEncryptionKey;
	(void)hAuthenticationKey;
	return p11_session_end(s, CKR_SAVED_STATE_INVALID);
}

CK_RV C_CopyObject(CK_SESSION_HANDLE hSession, CK_OBJECT_HANDLE hObject, CK_ATTRIBUTE_PTR pTemplate, CK_ULONG ulCount, CK_OBJECT_HANDLE_PTR phNewObject)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)hObject;
	(void)pTemplate;
	(void)ulCount;
	(void)phNewObject;
	return p11_session_end(s, CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_SignRecoverInit(CK_SESSION_HANDLE hSession, CK_MECHANISM_PTR pMechanism, CK_OBJECT_HANDLE hKey)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pMechanism;
	(void)hKey;
	return p11_session_end(s, CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_SignRecover(CK_SESSION_HANDLE hSession, CK_BYTE_PTR pData, CK_ULONG ulDataLen, CK_BYTE_PTR pSignature, CK_ULONG_PTR pulSignatureLen)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pData;
	(void)ulDataLen;
	(void)pSignature;
	(void)pulSignatureLen;
	return p11_session_end(s, CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_VerifyRecoverInit(CK_SESSION_HANDLE hSession, CK_MECHANISM_PTR pMechanism, CK_OBJECT_HANDLE hKey)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pMechanism;
	(void)hKey;
	return p11_session_end(s, CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_VerifyRecover(CK_SESSION_HANDLE hSession, CK_BYTE_PTR pSignature, CK_ULONG ulSignatureLen, CK_BYTE_PTR pData, CK_ULONG_PTR pulDataLen)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pSignature;
	(void)ulSignatureLen;
	(void)pData;
	(void)pulDataLen;
	return p11_session_end(s, CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_DigestEncryptUpdate(CK_SESSION_HANDLE hSession, CK_BYTE_PTR pPart, CK_ULONG ulPartLen, CK_BYTE_PTR pEncryptedPart, CK_ULONG_PTR pulEncryptedPartLen)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pPart;
	(void)ulPartLen;
	(void)pEncryptedPart;
	(void)pulEncryptedPartLen;
	return p11_session_end(s, CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_DecryptDigestUpdate(CK_SESSION_HANDLE hSession, CK_BYTE_PTR pEncryptedPart, CK_ULONG ulEncryptedPartLen, CK_BYTE_PTR pPart, CK_ULONG_PTR pulPartLen)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pEncryptedPart;
	(void)ulEncryptedPartLen;
	(void)pPart;
	(void)pulPartLen;
	return p11_session_end(s, CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_SignEncryptUpdate(CK_SESSION_HANDLE hSession, CK_BYTE_PTR pPart, CK_ULONG ulPartLen, CK_BYTE_PTR pEncryptedPart, CK_ULONG_PTR pulEncryptedPartLen)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pPart;
	(void)ulPartLen;
	(void)pEncryptedPart;
	(void)pulEncryptedPartLen;
	return p11_session_end(s, CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_DecryptVerifyUpdate(CK_SESSION_HANDLE hSession, CK_BYTE_PTR pEncryptedPart, CK_ULONG ulEncryptedPartLen, CK_BYTE_PTR pPart, CK_ULONG_PTR pulPartLen)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pEncryptedPart;
	(void)ulEncryptedPartLen;
	(void)pPart;
	(void)pulPartLen;
	return p11_session_end(s, CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_GenerateKey(CK_SESSION_HANDLE hSession, CK_MECHANISM_PTR pMechanism, CK_ATTRIBUTE_PTR pTemplate, CK_ULONG ulCount, CK_OBJECT_HANDLE_PTR phKey)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pMechanism;
	(void)pTemplate;
	(void)ulCount;
	(void)phKey;
	return p11_session_end(s, CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_GenerateKeyPair(CK_SESSION_HANDLE hSession, CK_MECHANISM_PTR pMechanism, CK_ATTRIBUTE_PTR pPublicKeyTemplate, CK_ULONG ulPublicKeyAttributeCount, CK_ATTRIBUTE_PTR pPrivateKeyTemplate, CK_ULONG ulPrivateKeyAttributeCount, CK_OBJECT_HANDLE_PTR phPublicKey, CK_OBJECT_HANDLE_PTR phPrivateKey)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pMechanism;
	(void)pPublicKeyTemplate;
	(void)ulPublicKeyAttributeCount;
	(void)pPrivateKeyTemplate;
	(void)ulPrivateKeyAttributeCount;
	(void)phPublicKey;
	(void)phPrivateKey;
	return p11_session_end(s, CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_WrapKey(CK_SESSION_HANDLE hSession, CK_MECHANISM_PTR pMechanism, CK_OBJECT_HANDLE hWrappingKey, CK_OBJECT_HANDLE hKey, CK_BYTE_PTR pWrappedKey, CK_ULONG_PTR pulWrappedKeyLen)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pMechanism;
	(void)hWrappingKey;
	(void)hKey;
	(void)pWrappedKey;
	(void)pulWrappedKeyLen;
	return p11_session_end(s, CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_UnwrapKey(CK_SESSION_HANDLE hSession, CK_MECHANISM_PTR pMechanism, CK_OBJECT_HANDLE hUnwrappingKey, CK_BYTE_PTR pWrappedKey, CK_ULONG ulWrappedKeyLen, CK_ATTRIBUTE_PTR pTemplate, CK_ULONG ulAttributeCount, CK_OBJECT_HANDLE_PTR phKey)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pMechanism;
	(void)hUnwrappingKey;
	(void)pWrappedKey;
	(void)ulWrappedKeyLen;
	(void)pTemplate;
	(void)ulAttributeCount;
	(void)phKey;
	return p11_session_end(s, CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_DeriveKey(CK_SESSION_HANDLE hSession, CK_MECHANISM_PTR pMechanism, CK_OBJECT_HANDLE hBaseKey, CK_ATTRIBUTE_PTR pTemplate, CK_ULONG ulAttributeCount, CK_OBJECT_HANDLE_PTR phKey)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pMechanism;
	(void)hBaseKey;
	(void)pTemplate;
	(void)ulAttributeCount;
	(void)phKey;
	return p11_session_end(s, CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_SeedRandom(CK_SESSION_HANDLE hSession, CK_BYTE_PTR pSeed, CK_ULONG ulSeedLen)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	(void)pSeed;
	(void)ulSeedLen;
	return p11_session_end(s, CKR_RANDOM_SEED_NOT_SUPPORTED);
}

CK_RV C_GetFunctionStatus(CK_SESSION_HANDLE hSession)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	return p11_session_end(s, CKR_FUNCTION_NOT_PARALLEL);
}

CK_RV C_CancelFunction(CK_SESSION_HANDLE hSession)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(hSession, &s); if (rv) return rv;
	return p11_session_end(s, CKR_FUNCTION_NOT_PARALLEL);
}

CK_RV C_WaitForSlotEvent(CK_FLAGS flags, CK_SLOT_ID_PTR pSlot, CK_VOID_PTR pReserved)
{
	CK_RV rv = p11_global_begin(); if (rv) return rv;
	(void)flags;
	(void)pSlot;
	(void)pReserved;
	return p11_global_end(pReserved || !pSlot || (flags & ~CKF_DONT_BLOCK) ? CKR_ARGUMENTS_BAD : flags & CKF_DONT_BLOCK ? CKR_NO_EVENT : CKR_FUNCTION_NOT_SUPPORTED);
}

#undef CK_NEED_ARG_LIST
#define CK_PKCS11_FUNCTION_INFO(name) .name = name,
static CK_FUNCTION_LIST functions = { .version = {2,40},
#include "pkcs11/pkcs11f.h"
};
#undef CK_PKCS11_FUNCTION_INFO

CK_RV C_GetFunctionList(CK_FUNCTION_LIST_PTR_PTR result)
{
	if (!result) return CKR_ARGUMENTS_BAD;
	*result = &functions; return CKR_OK;
}

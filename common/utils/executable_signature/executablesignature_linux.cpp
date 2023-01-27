#include "executablesignature_linux.h"

#include <codecvt>
#include <stdio.h>
#include <stdlib.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/evp.h>

#include "boost/filesystem/path.hpp"

#include "executable_signature.h"
#include "../../../common/utils/openssl_utils.h"

// key_pub.txt is generated by build_all.py from key.pub
// The syntax used in key_pub.txt utilizes the raw string literals feature of the C++11 standard.
// - https://www.stroustrup.com/C++11FAQ.html#raw-strings
#ifdef USE_SIGNATURE_CHECK
const char* g_PublicKeyData =
#include "../../../common/keys/linux/key_pub.txt"
;
#else
// key_pub.txt won't be generated for non-signed builds
const char* g_PublicKeyData = "";
#endif


ExecutableSignaturePrivate::ExecutableSignaturePrivate(ExecutableSignature* const q) : ExecutableSignaturePrivateBase(q)
{
}

ExecutableSignaturePrivate::~ExecutableSignaturePrivate()
{
}

// https://wiki.openssl.org/index.php/EVP_Signing_and_Verifying#Asymmetric_Key
// https://wiki.openssl.org/index.php/EVP#Working_with_EVP_PKEYs
bool ExecutableSignaturePrivate::verify(const std::string& exePath)
{
    std::string pubKeyBytes(g_PublicKeyData);

    // key.pub is 800 bytes on disk
    if (pubKeyBytes.size() > 800)
    {
        lastError_ << "Invalid public key, size is too large: " << pubKeyBytes.size() << " bytes";
        return false;
    }

    // read public key into openssl bio abstraction
    // TODO BIO_new_mem_buf?
    /*
    // https://stackoverflow.com/questions/11886262
    BIO *bufio;
    RSA *rsa
    bufio = BIO_new_mem_buf((void*)pem_key_buffer, pem_key_buffer_len);
    PEM_read_bio_RSAPublicKey(bufio, &rsa, 0, NULL);
    */
    // TODO what is wsl
    wsl::EvpBioCharBuf bioPublicKey;
    if (!bioPublicKey.isValid())
    {
        lastError_ << "Failed to allocate an OpenSSL BIO buffer";
        return false;
    }

    if (bioPublicKey.write(pubKeyBytes.data(), pubKeyBytes.length()) <= 0)
    {
        lastError_ << "Failed to write public key resource to bio";
        return false;
    }

    // Calculate SHA256 digest for datafile
    FILE* datafile = fopen(exePath.c_str() , "rb");
    if (datafile == NULL)
    {
        lastError_ << "Failed to open executable for reading: " << errno;
        return false;
    }

    // https://wiki.openssl.org/index.php/EVP_Message_Digests
    EVP_MD_CTX *mdctx;

    mdctx = EVP_MD_CTX_new();
    if (mdctx == NULL) {
        lastError_ << "Failed to create SHA256 context";
        return false;
    }

    if (EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL) != 1) {
    //EVP_PKEY key;
    //if (EVP_DigestSignInit(mdctx, NULL, EVP_sha256(), NULL, key) != 1) {
    //if (EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, key) != 1) {
        lastError_ << "Failed to init SHA256 digest";
        return false;
    }

    const size_t fileDataSize = 65536;
    std::unique_ptr<unsigned char[]> fileData(new unsigned char[fileDataSize]);

    // Read binary data in chunks and feed it to OpenSSL SHA256
    size_t bytesRead = 0;
    while ((bytesRead = fread(fileData.get(), 1, fileDataSize, datafile)))
    {
        if (EVP_DigestUpdate(mdctx, fileData.get(), bytesRead) != 1) {
            lastError_ << "Failed to update SHA256 digest";
            return false;
        }
    }

    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int digest_len = 0;
    if (1 != EVP_DigestFinal_ex(mdctx, digest, &digest_len)) {
        lastError_ << "Failed to finalize SHA256 digest";
        return false;
    }

    EVP_MD_CTX_free(mdctx);
    fclose(datafile);

    // RSA
    // https://www.openssl.org/docs/man3.0/man3/EVP_PKEY_verify.html

    EVP_PKEY *pkey;

    // Load RSA public key from bio
    pkey = PEM_read_bio_PUBKEY(bioPublicKey.getBIO(), NULL, NULL, NULL);
    //pkey = PEM_read_bio_PUBKEY_ex(bioPublicKey, NULL, NULL, NULL, NULL, NULL);
    if (pkey == NULL) {
        lastError_ << "Failed to load RSA public key";
        return false;
    }

    EVP_PKEY_CTX *ctx;

    ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    if (ctx == NULL) {
        lastError_ << "Failed to create RSA context";
        return false;
    }

    if (EVP_PKEY_verify_init(ctx) != 1) {
        lastError_ << "Failed to init RSA verify";
        return false;
    }

    if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) != 1) {
        lastError_ << "Failed to init RSA padding";
        return false;
    }

    if (EVP_PKEY_CTX_set_signature_md(ctx, EVP_sha256()) != 1) {
        lastError_ << "Failed to set RSA signature digest";
        return false;
    }

    if (EVP_PKEY_check(ctx) != 1) {
        lastError_ << "Failed to check RSA context";
        return false;
    }

    boost::filesystem::path path(exePath);
    std::ostringstream stream;
    stream << path.parent_path().native() << "/signatures/" << path.stem().native() << ".sig";
    std::string sigFile = stream.str();

    // Read signature from file
    FILE* sign = fopen(sigFile.c_str() , "r");
    if (sign == NULL)
    {
        lastError_ << "Failed to open signature file (" << sigFile << ") for reading: " << errno;
        return false;
    }

    bytesRead = fread(fileData.get(), 1, fileDataSize, sign);
    fclose(sign);

    // The RSA signature for the 4096-bit RSA key we use is 512 bytes.
    if (bytesRead != 512)
    {
        lastError_ << "Signature file is an invalid size, or failed to read entire file. Expected 512 bytes, read " << bytesRead << ".";
        return false;
    }

    // Decrypt signature and verify it matches with the digest calculated from data file.
    // RSA signature is in fileData
    int result = EVP_PKEY_verify(ctx, fileData.get(), bytesRead, digest, digest_len);

    EVP_PKEY_CTX_free(ctx);

    if (result != 1) {
        lastError_ << "Executable's signature does not match signature file";
    }

    return (result == 1);
}

bool ExecutableSignaturePrivate::verify(const std::wstring& exePath)
{
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    std::string converted = converter.to_bytes(exePath);
    return verify(converted);
}

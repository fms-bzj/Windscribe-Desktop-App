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

    EVP_MD_CTX *ctx;

    ctx = EVP_MD_CTX_new();
    if (ctx == NULL) {
        lastError_ << "Failed to init SHA256 context";
        return false;
    }

    if (1 != EVP_DigestInit_ex(ctx, EVP_sha256(), NULL)) {
        lastError_ << "Failed to init SHA256 digest";
        return false;
    }

    const size_t fileDataSize = 65536;
    std::unique_ptr<unsigned char[]> fileData(new unsigned char[fileDataSize]);

    // Read binary data in chunks and feed it to OpenSSL SHA256
    size_t bytesRead = 0;
    while ((bytesRead = fread(fileData.get(), 1, fileDataSize, datafile)))
    {
        if (1 != EVP_DigestUpdate(ctx, fileData.get(), bytesRead)) {
            lastError_ << "Failed to update SHA256 digest";
            return false;
        }
    }

    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (1 != EVP_DigestFinal_ex(ctx, *digest, SHA256_DIGEST_LENGTH)) {
        lastError_ << "Failed to finalize SHA256 digest";
        return false;
    }

    EVP_MD_CTX_free(ctx);
    fclose(datafile);

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

    // Verify that calculated digest and signature match
    RSA* rsa_pubkey = PEM_read_bio_RSA_PUBKEY(bioPublicKey.getBIO(), NULL, NULL, NULL);
    if (rsa_pubkey == NULL)
    {
        lastError_ << "Failed to read the RSA public key";
        return false;
    }

    // Decrypt signature and verify it matches with the digest calculated from data file.
    int result = RSA_verify(NID_sha256, digest, SHA256_DIGEST_LENGTH, fileData.get(), bytesRead, rsa_pubkey);
    RSA_free(rsa_pubkey);

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

/*
Trigglow Dynamic Delay for OBS
Copyright (C) 2026 Trigglow (VirosMs)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "spill-file.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/vfs.h>
#endif
#ifdef __APPLE__
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonRandom.h>
#elif defined(TRIGGLOW_SPILL_OPENSSL)
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif
#endif

namespace trigglow::relay {

namespace {

constexpr char kPrefix[] = "trigglow-dd-spill-";
constexpr char kSuffix[] = ".tmp";
// The keystream is made this many bytes at a time.
constexpr size_t kChunkBytes = 64 * 1024;

#if defined(_WIN32) || defined(__APPLE__) || defined(TRIGGLOW_SPILL_OPENSSL)
constexpr bool kHaveCrypto = true;
#else
constexpr bool kHaveCrypto = false;
#endif

bool RandomBytes(uint8_t *data, size_t bytes)
{
#ifdef _WIN32
	return BCRYPT_SUCCESS(
		BCryptGenRandom(nullptr, data, static_cast<ULONG>(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
#elif defined(__APPLE__)
	return CCRandomGenerateBytes(data, bytes) == kCCSuccess;
#elif defined(TRIGGLOW_SPILL_OPENSSL)
	return RAND_bytes(data, static_cast<int>(bytes)) == 1;
#else
	(void)data;
	(void)bytes;
	return false;
#endif
}

void Wipe(void *data, size_t bytes)
{
	volatile uint8_t *p = static_cast<volatile uint8_t *>(data);
	while (bytes-- > 0)
		*p++ = 0;
}

// AES-256 on whole 16-byte blocks (ECB): the counter blocks go in, the
// keystream comes out.
class Aes256 {
public:
	Aes256() = default;
	Aes256(const Aes256 &) = delete;
	Aes256 &operator=(const Aes256 &) = delete;
	~Aes256()
	{
#ifdef _WIN32
		if (key_)
			BCryptDestroyKey(key_);
		if (alg_)
			BCryptCloseAlgorithmProvider(alg_, 0);
#elif defined(__APPLE__)
		if (ref_)
			CCCryptorRelease(ref_);
#elif defined(TRIGGLOW_SPILL_OPENSSL)
		if (ctx_)
			EVP_CIPHER_CTX_free(ctx_);
#endif
	}

	bool Init(const uint8_t key[32])
	{
#ifdef _WIN32
		if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg_, BCRYPT_AES_ALGORITHM, nullptr, 0)))
			return false;
		if (!BCRYPT_SUCCESS(
			    BCryptSetProperty(alg_, BCRYPT_CHAINING_MODE,
					      reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_ECB)),
					      sizeof(BCRYPT_CHAIN_MODE_ECB), 0)))
			return false;
		return BCRYPT_SUCCESS(
			BCryptGenerateSymmetricKey(alg_, &key_, nullptr, 0, const_cast<PUCHAR>(key), 32, 0));
#elif defined(__APPLE__)
		return CCCryptorCreate(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, key, kCCKeySizeAES256, nullptr,
				       &ref_) == kCCSuccess;
#elif defined(TRIGGLOW_SPILL_OPENSSL)
		ctx_ = EVP_CIPHER_CTX_new();
		if (!ctx_ || EVP_EncryptInit_ex(ctx_, EVP_aes_256_ecb(), nullptr, key, nullptr) != 1)
			return false;
		return EVP_CIPHER_CTX_set_padding(ctx_, 0) == 1;
#else
		(void)key;
		return false;
#endif
	}

	// `bytes` is a multiple of 16; `in` and `out` don't overlap.
	bool Encrypt(const uint8_t *in, uint8_t *out, size_t bytes)
	{
#ifdef _WIN32
		ULONG done = 0;
		return BCRYPT_SUCCESS(BCryptEncrypt(key_, const_cast<PUCHAR>(in), static_cast<ULONG>(bytes), nullptr,
						    nullptr, 0, out, static_cast<ULONG>(bytes), &done, 0)) &&
		       done == bytes;
#elif defined(__APPLE__)
		size_t done = 0;
		return CCCryptorUpdate(ref_, in, bytes, out, bytes, &done) == kCCSuccess && done == bytes;
#elif defined(TRIGGLOW_SPILL_OPENSSL)
		int done = 0;
		return EVP_EncryptUpdate(ctx_, out, &done, in, static_cast<int>(bytes)) == 1 &&
		       static_cast<size_t>(done) == bytes;
#else
		(void)in;
		(void)out;
		(void)bytes;
		return false;
#endif
	}

private:
#ifdef _WIN32
	BCRYPT_ALG_HANDLE alg_ = nullptr;
	BCRYPT_KEY_HANDLE key_ = nullptr;
#elif defined(__APPLE__)
	CCCryptorRef ref_ = nullptr;
#elif defined(TRIGGLOW_SPILL_OPENSSL)
	EVP_CIPHER_CTX *ctx_ = nullptr;
#endif
};

void PutBe64(uint8_t *at, uint64_t value)
{
	for (int i = 7; i >= 0; --i) {
		at[i] = static_cast<uint8_t>(value & 0xff);
		value >>= 8;
	}
}

uint64_t GetBe64(const uint8_t *at)
{
	uint64_t value = 0;
	for (int i = 0; i < 8; ++i)
		value = (value << 8) | at[i];
	return value;
}

// XORs `in` into `out` with the keystream that starts at counter (hi, lo);
// `in` may be `out`. `ctr` and `ks` are kChunkBytes scratch buffers.
bool CtrXor(Aes256 &aes, uint64_t hi, uint64_t lo, const uint8_t *in, uint8_t *out, size_t bytes, uint8_t *ctr,
	    uint8_t *ks)
{
	size_t done = 0;
	while (done < bytes) {
		const size_t n = std::min(kChunkBytes, bytes - done);
		const size_t blocks = (n + 15) / 16;
		for (size_t j = 0; j < blocks; ++j) {
			const uint64_t l = lo + j;
			PutBe64(ctr + 16 * j, l < lo ? hi + 1 : hi);
			PutBe64(ctr + 16 * j + 8, l);
		}
		if (!aes.Encrypt(ctr, ks, blocks * 16))
			return false;
		for (size_t i = 0; i < n; ++i)
			out[done + i] = static_cast<uint8_t>(in[done + i] ^ ks[i]);
		const uint64_t next = lo + blocks;
		if (next < lo)
			++hi;
		lo = next;
		done += n;
	}
	return true;
}

std::string HexOf(const uint8_t *data, size_t bytes)
{
	static const char kDigits[] = "0123456789abcdef";
	std::string out;
	for (size_t i = 0; i < bytes; ++i) {
		out.push_back(kDigits[data[i] >> 4]);
		out.push_back(kDigits[data[i] & 15]);
	}
	return out;
}

bool IsSpillName(const std::string &name)
{
	const size_t prefix = sizeof(kPrefix) - 1;
	const size_t suffix = sizeof(kSuffix) - 1;
	return name.size() > prefix + suffix && name.compare(0, prefix, kPrefix) == 0 &&
	       name.compare(name.size() - suffix, suffix, kSuffix) == 0;
}

#ifdef _WIN32
std::wstring Widen(const std::string &text)
{
	if (text.empty())
		return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	std::wstring out(static_cast<size_t>(std::max(n, 0)), L'\0');
	if (n > 0)
		MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
	return out;
}

std::string Narrow(const std::wstring &text)
{
	if (text.empty())
		return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
					  nullptr);
	std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
	if (n > 0)
		WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr,
				    nullptr);
	return out;
}
#endif

std::string JoinPath(const std::string &dir, const std::string &name)
{
	if (dir.empty())
		return name;
	const char last = dir.back();
	if (last == '/' || last == '\\')
		return dir + name;
#ifdef _WIN32
	return dir + "\\" + name;
#else
	return dir + "/" + name;
#endif
}

} // namespace

bool SpillCryptoAvailable()
{
	return kHaveCrypto;
}

bool AesCtrXor(const uint8_t key[32], const uint8_t counter[16], uint8_t *data, size_t bytes)
{
	Aes256 aes;
	if (!aes.Init(key))
		return false;
	std::vector<uint8_t> ctr(kChunkBytes);
	std::vector<uint8_t> ks(kChunkBytes);
	return CtrXor(aes, GetBe64(counter), GetBe64(counter + 8), data, data, bytes, ctr.data(), ks.data());
}

struct SpillFile::Impl {
#ifdef _WIN32
	HANDLE file = INVALID_HANDLE_VALUE;
#else
	int fd = -1;
#endif
	Aes256 aes;
	uint64_t nonce = 0;
	std::vector<uint8_t> ctr = std::vector<uint8_t>(kChunkBytes);
	std::vector<uint8_t> ks = std::vector<uint8_t>(kChunkBytes);
	std::vector<uint8_t> out = std::vector<uint8_t>(kChunkBytes);

	~Impl()
	{
#ifdef _WIN32
		if (file != INVALID_HANDLE_VALUE)
			CloseHandle(file); // FILE_FLAG_DELETE_ON_CLOSE: the file is gone.
#else
		if (fd >= 0)
			close(fd); // Already unlinked: the space is freed.
#endif
		Wipe(&nonce, sizeof(nonce));
	}

	bool PWrite(uint64_t offset, const uint8_t *data, size_t bytes)
	{
		while (bytes > 0) {
#ifdef _WIN32
			OVERLAPPED ov = {};
			ov.Offset = static_cast<DWORD>(offset & 0xffffffffu);
			ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
			DWORD written = 0;
			const DWORD n = static_cast<DWORD>(std::min<size_t>(bytes, 1u << 30));
			if (!WriteFile(file, data, n, &written, &ov) || written == 0)
				return false;
#else
			const ssize_t written = pwrite(fd, data, bytes, static_cast<off_t>(offset));
			if (written < 0 && errno == EINTR)
				continue;
			if (written <= 0)
				return false;
#endif
			data += written;
			offset += static_cast<uint64_t>(written);
			bytes -= static_cast<size_t>(written);
		}
		return true;
	}

	bool PRead(uint64_t offset, uint8_t *data, size_t bytes)
	{
		while (bytes > 0) {
#ifdef _WIN32
			OVERLAPPED ov = {};
			ov.Offset = static_cast<DWORD>(offset & 0xffffffffu);
			ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
			DWORD read = 0;
			const DWORD n = static_cast<DWORD>(std::min<size_t>(bytes, 1u << 30));
			if (!ReadFile(file, data, n, &read, &ov) || read == 0)
				return false;
#else
			const ssize_t read = pread(fd, data, bytes, static_cast<off_t>(offset));
			if (read < 0 && errno == EINTR)
				continue;
			if (read <= 0)
				return false;
#endif
			data += read;
			offset += static_cast<uint64_t>(read);
			bytes -= static_cast<size_t>(read);
		}
		return true;
	}
};

SpillFile::SpillFile(std::unique_ptr<Impl> impl, size_t blockBytes, std::string path)
	: impl_(std::move(impl)),
	  blockBytes_(blockBytes),
	  path_(std::move(path))
{
}

SpillFile::~SpillFile() = default;

std::unique_ptr<SpillFile> SpillFile::Create(size_t blockBytes, std::string &error, const std::string &directory)
{
	if (!kHaveCrypto) {
		error = "no encryption available in this build";
		return nullptr;
	}
	if (blockBytes == 0 || blockBytes % 16 != 0) {
		error = "bad block size";
		return nullptr;
	}
	auto impl = std::make_unique<Impl>();
	uint8_t key[32];
	uint8_t nonce[8];
	uint8_t name[8];
	if (!RandomBytes(key, sizeof(key)) || !RandomBytes(nonce, sizeof(nonce)) || !RandomBytes(name, sizeof(name))) {
		error = "no random numbers for the key";
		return nullptr;
	}
	const bool keyed = impl->aes.Init(key);
	Wipe(key, sizeof(key)); // Only the cipher's own key schedule keeps it.
	if (!keyed) {
		error = "couldn't set up the encryption";
		return nullptr;
	}
	impl->nonce = GetBe64(nonce);
	Wipe(nonce, sizeof(nonce));

	const std::string dir = directory.empty() ? TempDirectory() : directory;
#ifdef _WIN32
	const std::string file = std::string(kPrefix) + std::to_string(GetCurrentProcessId()) + "-" +
				 HexOf(name, sizeof(name)) + kSuffix;
	const std::string path = JoinPath(dir, file);
	// Nobody else may open it, and Windows deletes it when the last handle
	// closes -- also when the process ends in a crash. Not
	// FILE_ATTRIBUTE_TEMPORARY: that asks Windows to keep its pages in RAM
	// instead of writing them out, which is what the spill is for.
	impl->file = CreateFileW(Widen(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
				 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	if (impl->file == INVALID_HANDLE_VALUE) {
		error = "couldn't create a file in the temp folder (error " + std::to_string(GetLastError()) + ")";
		return nullptr;
	}
#else
	const std::string file = std::string(kPrefix) + std::to_string(static_cast<long>(getpid())) + "-" +
				 HexOf(name, sizeof(name)) + kSuffix;
	const std::string path = JoinPath(dir, file);
	impl->fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (impl->fd < 0) {
		error = std::string("couldn't create a file in the temp folder (") + std::strerror(errno) + ")";
		return nullptr;
	}
	// Out of the folder at once: the data lives until the descriptor closes,
	// also when the process ends in a crash.
	unlink(path.c_str());
#endif
	return std::unique_ptr<SpillFile>(new SpillFile(std::move(impl), blockBytes, path));
}

bool SpillFile::Crypt(uint64_t blockId, size_t offsetInBlock, const uint8_t *in, uint8_t *out, size_t bytes)
{
	// Counter = nonce (high 64 bits) : the block's first 16-byte unit (low
	// 64 bits). Block numbers only grow, so every unit gets its own counter.
	const uint64_t lo = blockId * (blockBytes_ / 16) + offsetInBlock / 16;
	return CtrXor(impl_->aes, impl_->nonce, lo, in, out, bytes, impl_->ctr.data(), impl_->ks.data());
}

bool SpillFile::WriteSlot(uint64_t slot, uint64_t blockId, const uint8_t *data, size_t bytes)
{
	if (bytes > blockBytes_)
		return false;
	const uint64_t base = slot * blockBytes_;
	for (size_t done = 0; done < bytes; done += kChunkBytes) {
		const size_t n = std::min(kChunkBytes, bytes - done);
		if (!Crypt(blockId, done, data + done, impl_->out.data(), n) ||
		    !impl_->PWrite(base + done, impl_->out.data(), n))
			return false;
	}
	return true;
}

bool SpillFile::ReadSlot(uint64_t slot, uint64_t blockId, uint8_t *data, size_t bytes)
{
	if (bytes > blockBytes_ || !impl_->PRead(slot * blockBytes_, data, bytes))
		return false;
	return Crypt(blockId, 0, data, data, bytes);
}

bool SpillFile::ReadRawSlot(uint64_t slot, uint8_t *data, size_t bytes)
{
	return bytes <= blockBytes_ && impl_->PRead(slot * blockBytes_, data, bytes);
}

bool SpillFile::Truncate(uint64_t slots)
{
	const uint64_t size = slots * blockBytes_;
#ifdef _WIN32
	FILE_END_OF_FILE_INFO info = {};
	info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
	return SetFileInformationByHandle(impl_->file, FileEndOfFileInfo, &info, sizeof(info)) != 0;
#else
	return ftruncate(impl_->fd, static_cast<off_t>(size)) == 0;
#endif
}

uint64_t SpillFile::SizeOnDisk() const
{
#ifdef _WIN32
	LARGE_INTEGER size = {};
	return GetFileSizeEx(impl_->file, &size) ? static_cast<uint64_t>(size.QuadPart) : 0;
#else
	struct stat st = {};
	return fstat(impl_->fd, &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
#endif
}

std::string SpillFile::TempDirectory()
{
#ifdef _WIN32
	wchar_t buffer[MAX_PATH + 2] = {};
	const DWORD n = GetTempPathW(MAX_PATH + 1, buffer);
	if (n == 0 || n > MAX_PATH + 1)
		return ".";
	std::wstring dir(buffer, n);
	while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
		dir.pop_back();
	return Narrow(dir);
#else
	std::vector<std::string> candidates;
	if (const char *env = std::getenv("TMPDIR"); env && *env)
		candidates.emplace_back(env);
#ifdef __linux__
	// /tmp is a RAM-backed tmpfs on many distributions: spilling there would
	// only move the history from OBS's memory to the kernel's.
	candidates.emplace_back("/var/tmp");
#endif
	candidates.emplace_back("/tmp");
	std::string fallback;
	for (const std::string &dir : candidates) {
		if (access(dir.c_str(), W_OK | X_OK) != 0)
			continue;
#ifdef __linux__
		struct statfs fs = {};
		const bool inRam = statfs(dir.c_str(), &fs) == 0 &&
				   (static_cast<unsigned long>(fs.f_type) == 0x01021994ul ||
				    static_cast<unsigned long>(fs.f_type) == 0x858458f6ul);
		if (inRam) {
			if (fallback.empty())
				fallback = dir;
			continue;
		}
#endif
		return dir;
	}
	return fallback.empty() ? "/tmp" : fallback;
#endif
}

uint64_t SpillFile::FreeDiskBytes(const std::string &directory)
{
	const std::string dir = directory.empty() ? TempDirectory() : directory;
#ifdef _WIN32
	ULARGE_INTEGER available = {};
	if (!GetDiskFreeSpaceExW(Widen(dir).c_str(), &available, nullptr, nullptr))
		return 0;
	return static_cast<uint64_t>(available.QuadPart);
#else
	struct statvfs fs = {};
	if (statvfs(dir.c_str(), &fs) != 0)
		return 0;
	return static_cast<uint64_t>(fs.f_bavail) * static_cast<uint64_t>(fs.f_frsize);
#endif
}

int SpillFile::RemoveStale(const std::string &directory)
{
	const std::string dir = directory.empty() ? TempDirectory() : directory;
	int removed = 0;
#ifdef _WIN32
	WIN32_FIND_DATAW found = {};
	HANDLE search = FindFirstFileW(Widen(JoinPath(dir, std::string(kPrefix) + "*" + kSuffix)).c_str(), &found);
	if (search == INVALID_HANDLE_VALUE)
		return 0;
	do {
		const std::string name = Narrow(found.cFileName);
		if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsSpillName(name))
			continue;
		// A file a running OBS has open can't be deleted (sharing violation).
		if (DeleteFileW(Widen(JoinPath(dir, name)).c_str()))
			++removed;
	} while (FindNextFileW(search, &found));
	FindClose(search);
#else
	DIR *d = opendir(dir.c_str());
	if (!d)
		return 0;
	while (const dirent *entry = readdir(d)) {
		const std::string name = entry->d_name;
		if (IsSpillName(name) && unlink(JoinPath(dir, name).c_str()) == 0)
			++removed;
	}
	closedir(d);
#endif
	return removed;
}

} // namespace trigglow::relay

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <windows.h>

namespace ipasim::darwincred {

// Darwin uid_t/gid_t are 32-bit unsigned integers. Windows identities are SIDs,
// so ipaSim maps the terminal SID subauthority (the RID) into that 32-bit Darwin
// identifier space. This keeps the value OS-backed and stable for the current
// token instead of inventing a fixed simulator UID/GID.
inline bool sidTerminalRid(PSID Sid, std::uint32_t &Rid) {
  if (!Sid || !IsValidSid(Sid))
    return false;
  PUCHAR Count = GetSidSubAuthorityCount(Sid);
  if (!Count || *Count == 0)
    return false;
  PDWORD Value = GetSidSubAuthority(Sid, static_cast<DWORD>(*Count - 1));
  if (!Value)
    return false;
  Rid = static_cast<std::uint32_t>(*Value);
  return true;
}

inline bool openIdentityToken(bool Effective, HANDLE &Token) {
  Token = nullptr;
  if (Effective) {
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &Token))
      return true;
    if (GetLastError() != ERROR_NO_TOKEN)
      return false;
  }
  return OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token) != FALSE;
}

inline bool queryUserRid(bool Effective, std::uint32_t &Uid) {
  HANDLE Token = nullptr;
  if (!openIdentityToken(Effective, Token))
    return false;

  DWORD Required = 0;
  GetTokenInformation(Token, TokenUser, nullptr, 0, &Required);
  if (Required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    CloseHandle(Token);
    return false;
  }

  std::vector<std::uint8_t> Storage(Required);
  if (!GetTokenInformation(Token, TokenUser, Storage.data(), Required,
                           &Required)) {
    CloseHandle(Token);
    return false;
  }
  CloseHandle(Token);

  auto *User = reinterpret_cast<TOKEN_USER *>(Storage.data());
  return sidTerminalRid(User->User.Sid, Uid);
}

inline bool queryPrimaryGroupRid(bool Effective, std::uint32_t &Gid) {
  HANDLE Token = nullptr;
  if (!openIdentityToken(Effective, Token))
    return false;

  DWORD Required = 0;
  GetTokenInformation(Token, TokenPrimaryGroup, nullptr, 0, &Required);
  if (Required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    CloseHandle(Token);
    return false;
  }

  std::vector<std::uint8_t> Storage(Required);
  if (!GetTokenInformation(Token, TokenPrimaryGroup, Storage.data(), Required,
                           &Required)) {
    CloseHandle(Token);
    return false;
  }
  CloseHandle(Token);

  auto *Group = reinterpret_cast<TOKEN_PRIMARY_GROUP *>(Storage.data());
  return sidTerminalRid(Group->PrimaryGroup, Gid);
}

[[noreturn]] inline void failIdentityQuery(const char *Name) {
  const DWORD Error = GetLastError();
  std::fprintf(stderr,
               "[darwin-credential] fatal: %s could not derive Windows token identity (win32=%lu)\n",
               Name, static_cast<unsigned long>(Error));
  std::fflush(stderr);
  std::abort();
}

inline std::uint32_t getuid() {
  std::uint32_t Value = 0;
  if (!queryUserRid(false, Value))
    failIdentityQuery("getuid");
  return Value;
}

inline std::uint32_t geteuid() {
  std::uint32_t Value = 0;
  if (!queryUserRid(true, Value))
    failIdentityQuery("geteuid");
  return Value;
}

inline std::uint32_t getgid() {
  std::uint32_t Value = 0;
  if (!queryPrimaryGroupRid(false, Value))
    failIdentityQuery("getgid");
  return Value;
}

inline std::uint32_t getegid() {
  std::uint32_t Value = 0;
  if (!queryPrimaryGroupRid(true, Value))
    failIdentityQuery("getegid");
  return Value;
}

} // namespace ipasim::darwincred

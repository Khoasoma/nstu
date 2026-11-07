# Windows operator role evidence and kernel qualification

This standalone developer harness builds the actual provider under server/.
It does not install a broker, provision Windows roles, apply a token or
authorize the current application. Normal nonadmin permitted-role GUI
acceptance is REQUIRED before broker enforcement. Offline draft authoring
remains independent; legacy GUI SAS re-pairing, key custody and typed
operation authorization remain separately admitted work.

## Authentication and role policy

The provider captures the actual client of one complete local message-mode
pipe message. Exclusive move-only endpoint ownership/nonblocking receive lease,
oversize invalidation, outer-impersonation rejection, successful/fatal
RevertToSelf, TOKEN_QUERY|TOKEN_DUPLICATE original and query-only noninheritable
retained token, private origin and lifetime remain required.

Filtering history is an observation: checked TokenHasRestrictions nonzero
does not grant or ban a role. The aligned initialized four-byte field accepts
only exact BYTE1-native or DWORD4-documentary returned layouts, reads only the
validated prefix, and rejects errors/other lengths. Other token parsers do
not adopt this compatibility rule.

Current TokenRestrictedSids is separately queried/copied. Empty native x64
output measured eight bytes (DWORD count plus header padding); a four-byte
count-only prefix is also accepted. No unused one-element TOKEN_GROUPS array
is assumed. Nonempty output requires the complete offsetof/array/payload
bounds; every SID must be valid and point beyond the array. Errors, truncated
counts, bad pointers and inconsistent sizes deny. Returned sizes are logged
as measurements, not guessed from sizeof(TOKEN_GROUPS).

Every managed role requires:
- Valid actual impersonation identity, supported level, no anonymous or
  AppContainer fallback, active unlocked nonzero session, bound generation,
  strictly less than250ms original evidence age and unchanged principal/bindings.
- Qualified pinned local alias, current direct-user SAM membership with
  complete level0/flags0 result, and an enabled non-deny-only TokenGroups role
  SID. No nested-only/default/admin/Developer grant or linked-token upgrade.
- Actual AccessCheck on the captured query-only token against an in-memory
  descriptor with trusted owner/group and only the pinned role allow ACE.
  One specific role-use bit is requested, no null DACL, broad fallback ACE,
  MAXIMUM_ALLOWED or implicit owner/security/privilege right.
- API success AND AccessStatus/granted-bit success. Privilege-set sizing is
  bounded, at most one resize; malformed outputs/errors/unexpected
  privilege-backed grants deny. Windows performs current restricting-SID
  access semantics; manual SID intersection is not a substitute.

Teacher/Technician decisions are independent. Native access error aborts
evidence; kernel denial cannot be repaired by another token or broader ACE.
Before accepting any computed grant, one second complete direct-membership
snapshot follows all role kernel decisions. The original principal's SID/name
roundtrip and every configured pinned alias are checked after that snapshot.
Removal of any computed grant's membership denies the whole result. Fresh
membership can invalidate a grant; it cannot add one or repeat AccessCheck.
This adds at most five SAM/account calls (four with one configured role),
without pagination, retry or evidence recapture. No-grant and kernel-error
paths do not take the second snapshot.

Final full binding equality, session and original expiry checks follow the
added native work. Original captured_at is unchanged: age249ms can remain
valid, age250ms or backwards time denies, including at dispatch. No query
follows the final age check. No refresh/renewal or retroactive in-flight
cancellation is promised.

Evidence is initial data, not an indefinite bearer capability. Future broker
must call revalidate_for_dispatch at operation acceptance and use the returned
role_grants. Real pipe/file MIC/ACL and broker action/target/state checks still
apply; virtual role access is not installed-resource permission.

## Developer-only verification

Configure only tools/operator-authority-probe with C++20/WERROR and active
checks. Put C:\msys64\ucrt64\bin first on process PATH. Task-owned examples:

~~~powershell
cmake -S tools/operator-authority-probe -B build/auth-token-policy01-debug -G 'MinGW Makefiles' -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe -DCMAKE_BUILD_TYPE=Debug -DNSTU_ENABLE_WERROR=ON
cmake --build build/auth-token-policy01-debug --parallel 2
ctest --test-dir build/auth-token-policy01-debug --output-on-failure -V
cmake -S tools/operator-authority-probe -B build/auth-token-policy01-release -G 'MinGW Makefiles' -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe -DCMAKE_BUILD_TYPE=Release -DNSTU_ENABLE_WERROR=ON
cmake --build build/auth-token-policy01-release --parallel 2
ctest --test-dir build/auth-token-policy01-release --output-on-failure -V
~~~

Every exit matters. The dedicated Windows2022 MSVC workflow runs all
discovered CTest suites and requires the original four names; the kernel
surrogate mode is additional. The historical exact6c3d87f MSVC run passed4/4
and emitted D9025 (/DNDEBUG overridden with /UNDEBUG). It was not warning-free
or allowed-role acceptance; no suppression/assertion weakening is used.

## Evidence labels and limits

- Injected provider checks exercise source policy with synthetic principal/
  role/kernel facts. History alone no longer decides role; disabled/deny-only,
  nonmember, kernel denied/error, malformed/current restricting SID,
  generation/session/expiry/ownership failures remain. Positive roles are
  synthetic only.
- Post-kernel injected regressions cover observed direct-membership removal,
  principal remapping, alias recreation, incomplete/error snapshots, final
  session/binding/strict-age guards and dispatch without recapture. All24
  preceding injected case groups remain; the correction adds eight groups.
  These controls do not establish an instantaneous revocation guarantee.
- Native read-only checks query the own process and capture/revert uniquely
  owned self-pipe messages without configured roles; zero role grants remain.
- Owned privilege-filtered/no-restricting-SID token proves accurate history
  and base identity facts, not authority. It is never applied to a process/
  thread. Actual native returned-byte sizes are reported.
- Native adapter callbacks cover access/size/prefix/current-SID errors.
  DWORD4 compatibility is injected/documentary, not naturally observed.
- Native kernel-resource mode uses in-memory descriptors and a known
  non-role SID surrogate. Privilege-only history can allow that resource;
  mismatched restricting SID denies the second check; a matching list can
  allow both checks. None is a real managed Teacher/Technician grant.
  Error/output/privilege-set/pointer controls are labelled injected.

All native derived handles are owned and retired. No account/group/privilege
addition, live ACL/resource-policy or service/key/installed role change is
performed. No syscall observer returns role or authorization evidence.

Root's filtered execution context is not universal ordinary-user evidence.
Real normal sign-in/custom-role/current membership/kernel access through
the intended GUI must be demonstrated before broker integration. Session0/
inactive/no-room observations, synthetic grants and kernel surrogate access
cannot satisfy that acceptance.

SID/SAM/WTS/AccessCheck and dispatch are separate OS operations. The second
snapshot rejects changes across kernel work, and changes during that snapshot
when visible to the following principal/alias checks. A change after its last
observation can still race acceptance or dispatch. This is observed snapshot
freshness, not atomic OS revocation; effects already accepted are not cancelled.
Blocking calls and native allocation are not hard-cancelled by post-return
caps/age checks.
Actual IPC/resource mandatory integrity/ACL, minimal broker privileges,
real revocation/lock behavior, physical exam/AT/application acceptance and
Secure Setter issuance remain separate. No global input/security policy is
changed.

Official contracts:
- https://learn.microsoft.com/en-us/windows/win32/api/winnt/ne-winnt-token_information_class
- https://learn.microsoft.com/en-us/windows/win32/secauthz/restricted-tokens
- https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-accesscheck
- https://learn.microsoft.com/en-us/windows/win32/secauthz/sid-attributes-in-an-access-token
- https://learn.microsoft.com/en-us/windows/security/application-security/application-control/user-account-control/how-it-works
- https://learn.microsoft.com/en-us/windows/win32/secauthz/mandatory-integrity-control

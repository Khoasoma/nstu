# Windows operator authority provider qualification

This developer harness builds the actual reusable provider under server/,
not a copied authorization policy. It does not configure roles, install a
broker, load credentials, authorize the current UI, or provide an end-user
command-line sign-in/provisioning workflow.

Later admitted phases own normal GUI provisioning, a minimally privileged
broker account, credential custody, legacy-device GUI SAS re-pairing and
operation permissions. Offline draft authoring remains independent.

## Contract

Adoption transfers/invalidate the caller's exclusive local server-end
message-mode pipe HANDLE. No duplicate/borrowed endpoint reads are permitted
by the trusted caller contract; Windows cannot universally discover such
aliases. The wrapper exposes no raw handle. A nonblocking per-connection lease
rejects concurrent/reentrant receive across provider instances. Oversize,
truncation or EOF closes/invalidates the endpoint; tails never become later
requests.

Entry threads must have no impersonation token. Existing outer context or
unknown entry-query result rejects without altering it. Only provider-owned
pipe impersonation is reverted; failed RevertToSelf terminates the process.

Actual thread token opens TOKEN_QUERY | TOKEN_DUPLICATE. DuplicateTokenEx
retains TOKEN_QUERY only, TokenImpersonation, SecurityImpersonation and a
noninheritable handle. Evidence owns copied message/SID/logon/session/binding
storage and backend lifetime/private provider origin. Providers cannot be
copied/moved; foreign-provider and synthetic-to-native evidence rejects.

Role bindings default absent, including for administrators. Qualified local
alias names must match pinned valid SID, alias type and machine-domain identity
before/after query. TokenUser SID resolves to qualified Domain\User and
SID/type roundtrips before/after query. Direct domain-user members are supported;
failed lookup denies without a same-name local-account fallback.

NetUserGetLocalGroups uses local host, level 0, flags 0 and 32 KiB preferred
buffer. Only complete NERR_Success with consistent bounded output can grant.
ERROR_MORE_DATA/errors/invalid names/counts/bounds deny and free the buffer;
there is no NetLocalGroupGetMembers resume context. Post-return limits are
4096 names and 1 MiB buffer, not peak-allocation or call-deadline guarantees.

Every role requires direct membership plus enabled, non-deny-only token role
SID. Restricted means a successful checked TokenHasRestrictions ever-filtered
query, not merely an IsTokenRestricted restricting-SID list. Failed queries
always deny. TokenHasRestrictions alone accepts exact returned BYTE1 or DWORD4
layouts from an aligned initialized four-byte buffer, reading only the validated
prefix; every other length denies. BYTE1 is observed natively on this runtime;
DWORD4 is documentary/injected compatibility coverage only. Other token-value
parsers retain their exact layouts. Filtered/AppContainer/anonymous/identification-only
token states reject.
Teacher/Technician grants are independent; no Developer/default role, operation
matrix, raw dispatcher, signing authority or key API exists here.

Evidence is at most 250 ms old. Original token cleanup precedes validation;
receipt/revalidation check original monotonic age after blocking queries.
Session state is rechecked after membership at acceptance, then final expiry.
No renewal: expired evidence requires a new message. Session zero,
unknown/locked/inactive/disconnected state and generation/binding mismatch deny.
Revalidation queries retained token/current membership, not a new Windows logon.

Later broker callers must immediately revalidate_for_dispatch and use that
returned result's role_grants. Evidence grants() is its initial immutable
observation and must not be an indefinite dispatch credential.
AuthorizationResult::authorized() implies no installed service or dispatched
action. Evidence is not a wire/IPC bearer grant.

## Developer tests

Native Windows C++20 standalone target uses the existing warning module,
WERROR=ON and checks active under NDEBUG. No product-root/app target is built.
Native API declarations require Windows 8+; older inverted WTS lock flags are
not an accepted role path.

Put C:\msys64\ucrt64\bin first on process PATH to avoid the documented DLL
mismatch. Use uniquely owned build directories and check every exit code:

~~~powershell
cmake -S tools/operator-authority-probe -B build/auth01-fast -G 'MinGW Makefiles' -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe -DCMAKE_BUILD_TYPE=Debug -DNSTU_ENABLE_WERROR=ON
cmake --build build/auth01-fast --parallel 2
ctest --test-dir build/auth01-fast --output-on-failure
cmake -S tools/operator-authority-probe -B build/auth01-strict -G 'MinGW Makefiles' -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe -DCMAKE_BUILD_TYPE=Release -DNSTU_ENABLE_WERROR=ON
cmake --build build/auth01-strict --parallel 2
ctest --test-dir build/auth01-strict --output-on-failure
~~~

- auth01.injected_unit uses the actual provider with synthetic API facts.
  Positive grants are synthetic only. Cases cover direct/error membership,
  SID rebinding, unsupported tokens, outer context, ownership/lifetime,
  actual competing readers, late cleanup/query expiry and session locking
  during lookup. An owned child proves fatal reversion through its terminate
  handler; it cannot return successful authorization.
- auth01.native_read_only observes only own process/token/local session/direct
  local groups, then uses owned self-pipe capture/revert and oversized-tail
  invalidation. Output is counts/status, never user SIDs/accounts/group members
  or credentials. No managed role binding is supplied. The current native caller
  is ever-filtered, so its message capture is rejected as unsupported before
  retained authority; original capture/reversion/no-grant assertions remain.

Additional CTest modes preserve the original21 synthetic groups and separately
qualify the actual native adapter:

- auth01.native_owned_restrictions creates/query-checks only owned derived
  DISABLE_MAX_PRIVILEGE tokens and a query-only impersonation duplicate, never
  applying a token to a process/thread. The native adapter must observe
  ever-filtered=true and supported-shape=false even with zero restricting SIDs.
- auth01.native_query_errors injects only the read-only observer system-call
  boundary for TokenHasRestrictions: access-denied, invalid0/2/3/5 lengths,
  documentaryDWORD4 and BYTE1 trailing-byte sentinels. All other queries use
  Windows. No process-global hook exists, and this observation API returns no
  role/authorization evidence. Default provider capture always uses the real query.

Pipe fixtures create unique local names, current-user/SYSTEM object DACLs,
precise client rights without FILE_CREATE_PIPE_INSTANCE, noninheritable handles,
remote rejection and first-instance protection. No existing filesystem ACL or
policy changes. Local NPFS returns ERROR_PIPE_LOCAL from
GetNamedPipeClientComputerNameW; remote-name success/other errors deny.
Windows documents error 229 as "The pipe is local."

## Limits

Native observations never relabel existing/built-in groups as NSTU roles.
No native configured-role positive grant, installed broker authority, cross-user
identity, real revocation/service privilege/key-secrecy/GUI/exam-safety claim
follows from these tests.

Token groups are logon-era. Direct removal is seen on the next successful
membership check; a new grant still needs enabled role SID, normally new sign-in.
Domain translation can require domain availability; cached/offline domain
operator availability is not promised. Offline draft authoring is independent.

SID/SAM/WTS queries and dispatch are not atomic. Already accepted effects,
changes after the final check and session-lock scheduling remain races.
Future broker must invalidate managed generations/interactive queues on session
events and describe in-flight effects honestly.

Synchronous Windows calls can block before return. Buffer limits/final-age
rejection are not cancellation mechanisms. No global control-plane lock spans
pipe/SAM/WTS I/O. Native/test backends are trusted source dependencies, never
caller-controlled IPC choices.

Standalone MinGW success does not prove MSVC Release, full-product gates,
physical acceptance or deployment. Sanitizer qualification needs installed
runtime support; unavailable runtimes are not synthetic success.

Official references:
- https://learn.microsoft.com/en-us/windows/win32/api/lmaccess/nf-lmaccess-netusergetlocalgroups
- https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-lookupaccountsidw
- https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-duplicatetokenex
- https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-checktokenmembership
- https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-impersonatenamedpipeclient
- https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-reverttoself
- https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes--0-499-

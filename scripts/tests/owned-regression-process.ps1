$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# This bootstrap is outside the subsequently created worker job. A bootstrap
# failure never supplies worker/controller-tree cleanup confirmation.
$ownerClock = [System.Diagnostics.Stopwatch]::StartNew()
$ownerOriginUtc = [DateTime]::UtcNow.ToString('o')
$nativeSource = @'
using System;
using System.Collections.Generic;
using System.Collections.Concurrent;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

namespace NstuOwnedRegression {
    internal static class Native {
        [StructLayout(LayoutKind.Sequential)] public struct SecurityAttributes { public uint Length; public IntPtr Descriptor; public int Inherit; }
        [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct StartupInfo {
            public uint cb; public IntPtr reserved, desktop, title;
            public uint x,y,xSize,ySize,xChars,yChars,fill,flags;
            public ushort show,reservedBytes; public IntPtr reservedData,input,output,error;
        }
        [StructLayout(LayoutKind.Sequential)] public struct StartupInfoEx { public StartupInfo startup; public IntPtr attributes; }
        [StructLayout(LayoutKind.Sequential)] public struct ProcessInformation { public IntPtr process,thread; public uint pid,tid; }
        [StructLayout(LayoutKind.Sequential)] public struct BasicLimits {
            public long processTime,jobTime; public uint flags; public UIntPtr minWorking,maxWorking;
            public uint activeLimit; public UIntPtr affinity; public uint priority,scheduling;
        }
        [StructLayout(LayoutKind.Sequential)] public struct IoCounters { public ulong readOps,writeOps,otherOps,readBytes,writeBytes,otherBytes; }
        [StructLayout(LayoutKind.Sequential)] public struct ExtendedLimits { public BasicLimits basic; public IoCounters io; public UIntPtr processMemory,jobMemory,peakProcessMemory,peakJobMemory; }
        [StructLayout(LayoutKind.Sequential)] public struct Accounting { public long user,kernel,periodUser,periodKernel; public uint faults,total,active,terminated; }
        [StructLayout(LayoutKind.Sequential)] public struct FileTime { public uint low,high; public string Hex() { return (((ulong)high << 32) | low).ToString("X16"); } }
        [StructLayout(LayoutKind.Sequential)] public struct Overlapped { public UIntPtr internalValue,internalHigh; public uint offset,offsetHigh; public IntPtr eventHandle; }
        [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] public static extern IntPtr CreateJobObjectW(IntPtr attributes,string name);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool SetInformationJobObject(IntPtr job,uint kind,IntPtr info,uint length);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool QueryInformationJobObject(IntPtr job,uint kind,IntPtr info,uint length,out uint returned);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool AssignProcessToJobObject(IntPtr job,IntPtr process);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool IsProcessInJob(IntPtr process,IntPtr job,[MarshalAs(UnmanagedType.Bool)] out bool contained);
        [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool CreateProcessW(string app,StringBuilder command,IntPtr processAttributes,IntPtr threadAttributes,[MarshalAs(UnmanagedType.Bool)] bool inherit,uint flags,IntPtr env,string cwd,ref StartupInfoEx startup,out ProcessInformation info);
        [DllImport("kernel32.dll",SetLastError=true)] public static extern uint ResumeThread(IntPtr thread);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool TerminateProcess(IntPtr process,uint code);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool TerminateJobObject(IntPtr job,uint code);
        [DllImport("kernel32.dll",SetLastError=true)] public static extern uint WaitForSingleObject(IntPtr handle,uint milliseconds);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool GetExitCodeProcess(IntPtr process,out uint code);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool GetProcessTimes(IntPtr process,out FileTime created,out FileTime exited,out FileTime kernel,out FileTime user);
        [DllImport("kernel32.dll",SetLastError=true)] public static extern IntPtr OpenProcess(uint access,[MarshalAs(UnmanagedType.Bool)] bool inherit,uint pid);
        [DllImport("advapi32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool GetKernelObjectSecurity(IntPtr handle,uint information,IntPtr descriptor,uint length,out uint needed);
        [DllImport("advapi32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool SetKernelObjectSecurity(IntPtr handle,uint information,IntPtr descriptor);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool CreatePipe(out IntPtr read,out IntPtr write,ref SecurityAttributes attributes,uint size);
        [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] public static extern IntPtr CreateFileW(string path,uint access,uint share,ref SecurityAttributes attributes,uint creation,uint flags,IntPtr template);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool SetHandleInformation(IntPtr handle,uint mask,uint flags);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool GetHandleInformation(IntPtr handle,out uint flags);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool InitializeProcThreadAttributeList(IntPtr list,uint count,uint flags,ref UIntPtr size);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool UpdateProcThreadAttribute(IntPtr list,uint flags,UIntPtr attribute,IntPtr value,UIntPtr size,IntPtr previous,IntPtr returned);
        [DllImport("kernel32.dll",SetLastError=true)] public static extern void DeleteProcThreadAttributeList(IntPtr list);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool ReadFile(IntPtr file,IntPtr buffer,uint length,out uint read,IntPtr overlapped);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool DuplicateHandle(IntPtr sourceProcess,IntPtr source,IntPtr targetProcess,out IntPtr target,uint access,[MarshalAs(UnmanagedType.Bool)] bool inherit,uint options);
        [DllImport("kernel32.dll",SetLastError=true)] public static extern IntPtr GetCurrentProcess();
        [DllImport("kernel32.dll",SetLastError=true)] public static extern IntPtr GetCurrentThread();
        [DllImport("kernel32.dll",SetLastError=true)] public static extern IntPtr GetStdHandle(int kind);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool CancelSynchronousIo(IntPtr thread);
        [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] public static extern IntPtr CreateNamedPipeW(string name,uint access,uint mode,uint instances,uint outputSize,uint inputSize,uint timeout,ref SecurityAttributes attributes);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool ConnectNamedPipe(IntPtr pipe,IntPtr overlapped);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool GetOverlappedResult(IntPtr file,IntPtr overlapped,out uint transferred,[MarshalAs(UnmanagedType.Bool)] bool wait);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool CancelIoEx(IntPtr file,IntPtr overlapped);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool DisconnectNamedPipe(IntPtr pipe);
        [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] public static extern IntPtr CreateEventW(IntPtr attributes,[MarshalAs(UnmanagedType.Bool)] bool manual,[MarshalAs(UnmanagedType.Bool)] bool initial,string name);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool SetEvent(IntPtr handle);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool ResetEvent(IntPtr handle);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool GetNamedPipeClientProcessId(IntPtr pipe,out uint pid);
        [DllImport("advapi32.dll",SetLastError=true,CharSet=CharSet.Unicode)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool ConvertStringSecurityDescriptorToSecurityDescriptorW(string descriptor,uint revision,out IntPtr converted,out uint size);
        [DllImport("kernel32.dll",SetLastError=true)] public static extern IntPtr LocalFree(IntPtr memory);
    }

    internal sealed class ProtocolException : Exception { public ProtocolException(string message):base(message){} }
    internal static class Json {
        public static JavaScriptSerializer Serializer() { return new JavaScriptSerializer { MaxJsonLength=524288, RecursionLimit=32 }; }
        private sealed class Scanner {
            readonly string input; int pos;
            public Scanner(string text) { input=text; }
            void Fail() { throw new ProtocolException("Invalid control JSON at offset " + pos); }
            void White() { while(pos<input.Length && (input[pos]==' ' || input[pos]=='\r' || input[pos]=='\n' || input[pos]=='\t')) ++pos; }
            string String() {
                int start=pos++;
                while(pos<input.Length) {
                    char c=input[pos++];
                    if(c=='\\') { if(pos>=input.Length) Fail(); ++pos; }
                    else if(c=='"') { try { return Serializer().Deserialize<string>(input.Substring(start,pos-start)); } catch { Fail(); } }
                    else if(c<32) Fail();
                }
                Fail(); return null;
            }
            void Value(int depth) {
                if(depth>32) Fail(); White(); if(pos>=input.Length) Fail(); char c=input[pos];
                if(c=='{') {
                    ++pos; White(); var keys=new HashSet<string>(StringComparer.Ordinal);
                    if(pos<input.Length && input[pos]=='}') { ++pos; return; }
                    while(true) {
                        if(pos>=input.Length || input[pos]!='"') Fail(); string key=String(); if(!keys.Add(key)) Fail(); White();
                        if(pos>=input.Length || input[pos++]!=':') Fail(); Value(depth+1); White();
                        if(pos<input.Length && input[pos]=='}') { ++pos; return; } if(pos>=input.Length || input[pos++]!=',') Fail(); White();
                    }
                }
                if(c=='[') {
                    ++pos; White(); if(pos<input.Length && input[pos]==']') { ++pos; return; }
                    while(true) { Value(depth+1); White(); if(pos<input.Length && input[pos]==']') { ++pos; return; } if(pos>=input.Length || input[pos++]!=',') Fail(); }
                }
                if(c=='"') { String(); return; }
                int start=pos; while(pos<input.Length && ",]} \r\n\t".IndexOf(input[pos])<0) ++pos;
                string token=input.Substring(start,pos-start);
                if(token!="true" && token!="false" && token!="null" && !System.Text.RegularExpressions.Regex.IsMatch(token,@"^-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?$")) Fail();
            }
            public void Run() { Value(0); White(); if(pos!=input.Length) Fail(); }
        }
        public static Dictionary<string,object> Decode(byte[] bytes) {
            string text;
            try { text=new UTF8Encoding(false,true).GetString(bytes); } catch { throw new ProtocolException("Invalid control UTF-8"); }
            new Scanner(text).Run();
            try { var value=Serializer().DeserializeObject(text) as Dictionary<string,object>; if(value==null) throw new ProtocolException("Control object required"); return value; }
            catch(ProtocolException) { throw; } catch { throw new ProtocolException("Invalid control JSON"); }
        }
        public static byte[] Encode(object value) { return new UTF8Encoding(false,true).GetBytes(Serializer().Serialize(value)); }
    }

    internal sealed class Owner {
        readonly Stopwatch clock; readonly string originUtc,script;
        readonly object gate=new object(); readonly List<Dictionary<string,object>> operations=new List<Dictionary<string,object>>();
        readonly List<Dictionary<string,object>> primary=new List<Dictionary<string,object>>(), cleanupErrors=new List<Dictionary<string,object>>();
        readonly List<Handle> handles=new List<Handle>();
        readonly BlockingCollection<Dictionary<string,object>> messages=new BlockingCollection<Dictionary<string,object>>(256);
        readonly Stream input=Console.OpenStandardInput(), output=Console.OpenStandardOutput();
        readonly ManualResetEvent writerDone=new ManualResetEvent(false);
        Thread writer,reader,stdoutReader,stderrReader;
        Dictionary<string,object> request,testLiveness; string runId,nonce,generation,bindingHash,stopReason,kind="worker-exit",fault;
        volatile bool protocolFailed,terminal,outputOverflow;
        bool created,assigned,resumed,stdoutEof,stderrEof,rootSignaled,peerSignaled,rootReported,allHandlesClosed=true;
        uint pid,rawExit; uint? active,rootWaitResult; int? rootWaitError; bool hasRawExit; int sequence,dataFrames,inputFrames;
        long stdoutBytes,stderrBytes; double ownedAt=-1,readyAt=-1,stopAt=-1,cleanupFinishedAt=-1,normalCleanupOrigin=-1;
        int startupMs=30000,overallMs=600000,cleanupMs=10000,exitMs=2000;
        Handle job,process,thread,stdin,stdoutRead,stdoutWrite,stderrRead,stderrWrite,stdoutThread,stderrThread,peer;
        IntPtr attributeList,handleList,environment; bool attributeInitialized;
        ReadyPipe readiness;
        public Owner(string file,Stopwatch stopwatch,string utc) {
            script=file; clock=stopwatch; originUtc=utc;
            // Allocate the ownership containers before CreateProcessW. Once the
            // suspended child exists, recording its native handles allocates nothing.
            process=new Handle(this,IntPtr.Zero,"root-process"); thread=new Handle(this,IntPtr.Zero,"root-thread");
            handles.Add(process); handles.Add(thread);
        }
        static Dictionary<string,object> Map(params object[] values) { var map=new Dictionary<string,object>(StringComparer.Ordinal); for(int index=0;index<values.Length;index+=2) map.Add((string)values[index],values[index+1]); return map; }
        static string Hash(byte[] bytes) { using(var hash=SHA256.Create()) return BitConverter.ToString(hash.ComputeHash(bytes)).Replace("-",""); }
        static string String(Dictionary<string,object> map,string key) {
            object value; if(!map.TryGetValue(key,out value) || !(value is string)) throw new ProtocolException("String field required: "+key);
            string text=(string)value; if(text.IndexOf('\0')>=0) throw new ProtocolException("NUL string field: "+key);
            for(int index=0;index<text.Length;++index) {
                if(char.IsHighSurrogate(text[index])) { if(++index>=text.Length || !char.IsLowSurrogate(text[index])) throw new ProtocolException("UTF-16 field: "+key); }
                else if(char.IsLowSurrogate(text[index])) throw new ProtocolException("UTF-16 field: "+key);
            }
            return text;
        }
        static int Number(Dictionary<string,object> map,string key,int min,int max) {
            object value; if(!map.TryGetValue(key,out value) || (!(value is int) && !(value is long))) throw new ProtocolException("Integer field required: "+key);
            long number=Convert.ToInt64(value); if(number<min || number>max) throw new ProtocolException("Integer field bound: "+key); return (int)number;
        }
        static Dictionary<string,object> Object(Dictionary<string,object> map,string key) { object value; if(!map.TryGetValue(key,out value) || !(value is Dictionary<string,object>)) throw new ProtocolException("Object field required: "+key); return (Dictionary<string,object>)value; }
        static void Keys(Dictionary<string,object> map,params string[] keys) { if(map.Count!=keys.Length || keys.Any(key=>!map.ContainsKey(key))) throw new ProtocolException("Control schema field mismatch"); }
        void Error(List<Dictionary<string,object>> target,string area,string message,int? error,Exception caught) {
            var value=Map("kind",area,"message",message);
            if(error.HasValue) value.Add("win32Error",error.Value);
            if(caught!=null) value.Add("stack",caught.ToString());
            lock(gate) target.Add(value);
        }
        void Op(string operation,string role,bool ok,object returned,int error) {
            lock(gate) {
                // Repeated identical successful accounting polls are bounded
                // observations. Every changed result and every failure is kept.
                if(ok && role=="job-accounting" && operations.Count>0) {
                    var previous=operations[operations.Count-1];
                    if((string)previous["operation"]==operation && (string)previous["resourceRole"]==role && (bool)previous["ok"] && object.Equals(previous["returnValue"],returned)) {
                        previous["observations"]=previous.ContainsKey("observations")?Convert.ToInt32(previous["observations"])+1:2;
                        previous["controllerLastElapsedMs"]=clock.Elapsed.TotalMilliseconds; return;
                    }
                }
                operations.Add(Map("operation",operation,"resourceRole",role,"ok",ok,"returnValue",returned,"win32Error",ok?(object)null:error,"controllerElapsedMs",clock.Elapsed.TotalMilliseconds));
            }
        }
        void Check(bool ok,string operation,string role) {
            int error=ok?0:Marshal.GetLastWin32Error(); Op(operation,role,ok,ok,error);
            if(!ok) throw new Win32Exception(error,operation+" failed; native error "+error);
        }
        bool CleanupBool(bool ok,string operation,string role) {
            int error=ok?0:Marshal.GetLastWin32Error(); Op(operation,role,ok,ok,error);
            if(!ok) Error(cleanupErrors,operation,operation+" failed; native error "+error,error,null);
            return ok;
        }
        sealed class Handle {
            readonly Owner owner; public IntPtr Value; public readonly string Role;
            public Handle(Owner parent,IntPtr value,string role) { owner=parent; Value=value; Role=role; }
            public bool Close() {
                if(Value==IntPtr.Zero) return true;
                bool ok=Native.CloseHandle(Value); int error=ok?0:Marshal.GetLastWin32Error();
                if(ok) Value=IntPtr.Zero; else owner.allHandlesClosed=false;
                owner.Op("CloseHandle",Role,ok,ok,error);
                if(!ok) owner.Error(owner.cleanupErrors,"CloseHandle","CloseHandle failed: "+Role+"; native error "+error,error,null);
                return ok;
            }
        }
        Handle Own(IntPtr value,string role,bool invalidMinusOne) {
            if(value==IntPtr.Zero || (invalidMinusOne && value==new IntPtr(-1))) { int error=Marshal.GetLastWin32Error(); Op("CreateHandle",role,false,null,error); throw new Win32Exception(error,"Create handle failed: "+role+"; native error "+error); }
            var handle=new Handle(this,value,role); lock(gate) handles.Add(handle); Op("CreateHandle",role,true,null,0); return handle;
        }
        Handle ReserveHandle(string role) {
            var handle=new Handle(this,IntPtr.Zero,role); lock(gate) handles.Add(handle); return handle;
        }
        byte[] ReadExact(int length) {
            var bytes=new byte[length]; int offset=0;
            while(offset<length) { int got=input.Read(bytes,offset,length-offset); if(got==0) throw new ProtocolException("Private controller input EOF"); offset+=got; }
            return bytes;
        }
        void InputLoop() {
            try {
                while(!terminal) {
                    byte[] header=ReadExact(4); uint length=BitConverter.ToUInt32(header,0);
                    if(length==0 || length>262144 || ++inputFrames>256) throw new ProtocolException("Private command bound exceeded");
                    var frame=Json.Decode(ReadExact((int)length));
                    lock(gate) {
                        if(request==null) {
                            Keys(frame,"v","type","runId","nonce","generation","sourceBindingSha256","seq","application","argv","cwd","envEntries","budgetsMs","mode","sourceBinding","readiness","testFault");
                            if(Number(frame,"v",1,1)!=1 || String(frame,"type")!="LAUNCH" || Number(frame,"seq",1,1)!=1) throw new ProtocolException("First command must be LAUNCH1");
                            runId=String(frame,"runId"); nonce=String(frame,"nonce"); generation=String(frame,"generation"); bindingHash=String(frame,"sourceBindingSha256");
                            if(!System.Text.RegularExpressions.Regex.IsMatch(runId,@"^[a-f0-9]{32}$") || !System.Text.RegularExpressions.Regex.IsMatch(nonce,@"^[a-f0-9]{64}$") || !System.Text.RegularExpressions.Regex.IsMatch(generation,@"^[a-f0-9]{16}$") || !System.Text.RegularExpressions.Regex.IsMatch(bindingHash,@"^[A-F0-9]{64}$")) throw new ProtocolException("Invalid launch identity");
                            request=frame;
                        } else {
                            Keys(frame,"v","type","runId","nonce","generation","sourceBindingSha256","seq","reason");
                            if(Number(frame,"v",1,1)!=1 || String(frame,"type")!="STOP" || Number(frame,"seq",2,2)!=2 || stopReason!=null || String(frame,"runId")!=runId || String(frame,"nonce")!=nonce || String(frame,"generation")!=generation || String(frame,"sourceBindingSha256")!=bindingHash) throw new ProtocolException("Private stop identity/sequence mismatch");
                            string reason=String(frame,"reason");
                            if(!new[]{"forced-2000ms","execution-deadline","overall-deadline","startup-deadline","output-overflow","parent-abort"}.Contains(reason)) throw new ProtocolException("Invalid stop reason");
                            stopReason=reason;
                        }
                    }
                }
            } catch(Exception error) {
                if(!terminal) { protocolFailed=true; Error(primary,"protocol-failure",error.Message,null,error); lock(gate) { if(stopReason==null) stopReason="parent-abort"; } }
            }
        }
        void Send(string type,Dictionary<string,object> payload) {
            lock(gate) {
                if(type=="DATA" && ++dataFrames>8192) { outputOverflow=true; if(stopReason==null) stopReason="output-overflow"; return; }
                var frame=Map("v",1,"type",type,"runId",runId,"nonce",nonce,"generation",generation,"sourceBindingSha256",bindingHash,"seq",++sequence);
                foreach(var entry in payload) frame.Add(entry.Key,entry.Value);
                if(!messages.TryAdd(frame,100)) { outputOverflow=true; if(stopReason==null) stopReason="output-overflow"; }
            }
        }
        void OutputLoop() {
            try {
                foreach(var frame in messages.GetConsumingEnumerable()) {
                    byte[] bytes=Json.Encode(frame); int bound=(string)frame["type"]=="TERMINAL"?524288:131072;
                    if(bytes.Length>bound) throw new ProtocolException("Controller output frame bound exceeded");
                    byte[] prefix=BitConverter.GetBytes(bytes.Length); output.Write(prefix,0,4); output.Write(bytes,0,bytes.Length); output.Flush();
                }
            } catch(Exception error) { protocolFailed=true; Error(primary,"protocol-failure","Private controller output failed",null,error); lock(gate) { if(stopReason==null) stopReason="parent-abort"; } }
            finally { writerDone.Set(); }
        }
        static int Size(Type type) { return Marshal.SizeOf(type); }
        Dictionary<string,object> Abi() {
            if(IntPtr.Size!=8) throw new ProtocolException("x64 ABI required");
            var values=Map("startupInfo",Size(typeof(Native.StartupInfo)),"startupInfoEx",Size(typeof(Native.StartupInfoEx)),"processInformation",Size(typeof(Native.ProcessInformation)),
                "basicLimits",Size(typeof(Native.BasicLimits)),"ioCounters",Size(typeof(Native.IoCounters)),"extendedLimits",Size(typeof(Native.ExtendedLimits)),"accounting",Size(typeof(Native.Accounting)),"securityAttributes",Size(typeof(Native.SecurityAttributes)),"fileTime",Size(typeof(Native.FileTime)),"overlapped",Size(typeof(Native.Overlapped)));
            int[] expected={104,112,24,64,48,144,48,24,8,32}; int index=0;
            foreach(var value in values.Values) if(Convert.ToInt32(value)!=expected[index++]) throw new ProtocolException("Native structure size mismatch");
            if(Marshal.OffsetOf(typeof(Native.StartupInfoEx),"attributes").ToInt32()!=104 || Marshal.OffsetOf(typeof(Native.ProcessInformation),"pid").ToInt32()!=16 || Marshal.OffsetOf(typeof(Native.ExtendedLimits),"io").ToInt32()!=64 || Marshal.OffsetOf(typeof(Native.ExtendedLimits),"processMemory").ToInt32()!=112 || Marshal.OffsetOf(typeof(Native.Overlapped),"eventHandle").ToInt32()!=24) throw new ProtocolException("Native structure offset mismatch");
            return values;
        }
        void ValidateBinding() {
            var binding=Object(request,"sourceBinding"); Keys(binding,"bundleRoot","runtimeBundle","scenarioInputs","sha256");
            string directory=String(binding,"bundleRoot");
            if(Path.GetFullPath(directory)!=Path.GetFullPath(Path.GetDirectoryName(script))) throw new ProtocolException("Controller source directory mismatch");
            var runtime=Object(binding,"runtimeBundle"); Keys(runtime,"owned-regression-process.mjs","owned-regression-process.ps1","owned-regression-process-tests.mjs");
            foreach(var entry in runtime) if(Hash(File.ReadAllBytes(Path.Combine(directory,entry.Key)))!=(string)entry.Value) throw new ProtocolException("Copied owner source hash mismatch");
            var tuples=new List<string[]>();
            foreach(var role in new[]{"runtimeBundle","scenarioInputs"}) foreach(var entry in Object(binding,role)) {
                if(entry.Key.IndexOf('\\')>=0 || Path.IsPathRooted(entry.Key) || entry.Key.Split('/').Contains("..") || !(entry.Value is string) || !System.Text.RegularExpressions.Regex.IsMatch((string)entry.Value,@"^([a-fA-F0-9]{64}|ABSENT)$")) throw new ProtocolException("Source binding field mismatch");
                tuples.Add(new[]{role,entry.Key,(string)entry.Value});
                if(role=="scenarioInputs") {
                    string input=Path.Combine(String(request,"cwd"),entry.Key.Replace('/',Path.DirectorySeparatorChar));
                    if((string)entry.Value=="ABSENT") { if(File.Exists(input) || Directory.Exists(input)) throw new ProtocolException("Expected missing scenario source exists"); }
                    else if(!File.Exists(input) || Hash(File.ReadAllBytes(input))!=(string)entry.Value) throw new ProtocolException("Scenario source bytes changed");
                }
            }
            tuples.Sort((a,b)=> { int role=StringComparer.Ordinal.Compare(a[0],b[0]); return role!=0?role:StringComparer.Ordinal.Compare(a[1],b[1]); });
            string canonical=string.Concat(tuples.Select(tuple=>string.Join("\0",tuple)+"\0"));
            if(Hash(new UTF8Encoding(false,true).GetBytes(canonical))!=bindingHash || String(binding,"sha256")!=bindingHash) throw new ProtocolException("Source manifest digest mismatch");
        }
        static string Quote(string value) {
            var result=new StringBuilder("\""); int slashes=0;
            foreach(char c in value) { if(c=='\\') { ++slashes; continue; } if(c=='"') result.Append('\\',2*slashes+1).Append('"'); else result.Append('\\',slashes).Append(c); slashes=0; }
            return result.Append('\\',2*slashes).Append('"').ToString();
        }
        string[] Arguments() {
            object[] array=request["argv"] as object[]; if(array==null) throw new ProtocolException("Worker argv array required");
            return array.Select(value=>String(Map("argv",value),"argv")).ToArray();
        }
        void EnvironmentBlock() {
            object[] entries=request["envEntries"] as object[]; if(entries==null) throw new ProtocolException("Environment array required");
            var seen=new HashSet<string>(StringComparer.OrdinalIgnoreCase); var values=new List<KeyValuePair<string,string>>();
            foreach(object raw in entries) {
                var entry=raw as Dictionary<string,object>; if(entry==null) throw new ProtocolException("Environment entry required"); Keys(entry,"key","value");
                string key=String(entry,"key"),value=String(entry,"value");
                if(key.Length==0 || !seen.Add(key)) throw new ProtocolException("Empty/duplicate environment key");
                if(key.StartsWith("=",StringComparison.Ordinal)) {
                    if(!System.Text.RegularExpressions.Regex.IsMatch(key,@"^=[A-Za-z]:$") || !System.Text.RegularExpressions.Regex.IsMatch(value,@"^[A-Za-z]:[\\/]") || !value.Substring(0,2).Equals(key.Substring(1),StringComparison.OrdinalIgnoreCase)) throw new ProtocolException("Reserved drive environment field mismatch");
                } else if(key.IndexOf('=')>=0) throw new ProtocolException("Environment key field mismatch");
                values.Add(new KeyValuePair<string,string>(key,value));
            }
            values.Sort((a,b)=>StringComparer.OrdinalIgnoreCase.Compare(a.Key,b.Key));
            string block=string.Concat(values.Select(value=>value.Key+"="+value.Value+"\0"))+"\0"+(values.Count==0?"\0":"");
            byte[] bytes=Encoding.Unicode.GetBytes(block); if(bytes.Length>1048576) throw new ProtocolException("Environment byte bound exceeded");
            environment=Marshal.AllocHGlobal(bytes.Length); Marshal.Copy(bytes,0,environment,bytes.Length); Op("AllocateEnvironment","worker-environment",true,bytes.Length,0);
        }
        void Noninherit(IntPtr value,string role) { Check(Native.SetHandleInformation(value,1,0),"SetHandleInformation",role); uint flags; Check(Native.GetHandleInformation(value,out flags),"GetHandleInformation",role); if((flags&1)!=0) throw new ProtocolException("Excluded handle remained inheritable"); }
        void ValidateLivenessControl(string cwd,string[] args) {
            if(fault==null || !fault.StartsWith("liveness-",StringComparison.Ordinal)) return;
            if(args.Length!=4 || args[2]!="liveness-root" || String(request,"mode")!="normal") throw new ProtocolException("Liveness control actor/mode mismatch");
            string directory=Path.GetFullPath(cwd),temporary=Path.GetFullPath(Path.GetTempPath()).TrimEnd(Path.DirectorySeparatorChar);
            if(!StringComparer.OrdinalIgnoreCase.Equals(Path.GetDirectoryName(directory),temporary) || !Path.GetFileName(directory).StartsWith("nstu-owned-liveness-",StringComparison.Ordinal)) throw new ProtocolException("Liveness control requires one owned temporary leaf");
            testLiveness=Json.Decode(new UTF8Encoding(false,true).GetBytes(args[3])); Keys(testLiveness,"token","scenario","exitCode","foreignPID");
            if(!System.Text.RegularExpressions.Regex.IsMatch(String(testLiveness,"token"),@"^[a-f0-9]{64}$") || String(testLiveness,"scenario")!=fault ||
                (Number(testLiveness,"exitCode",0,259)!=0 && Number(testLiveness,"exitCode",0,259)!=259)) throw new ProtocolException("Liveness fixture configuration mismatch");
            if(fault=="liveness-foreign") { long target=Convert.ToInt64(testLiveness["foreignPID"]); if(target<1 || target>uint.MaxValue) throw new ProtocolException("Invalid foreign liveness identity"); }
            else if(testLiveness["foreignPID"]!=null) throw new ProtocolException("Unexpected foreign liveness target");
            string marker=Path.Combine(directory,".liveness-owner.json");
            var inputs=Object(Object(request,"sourceBinding"),"scenarioInputs");
            if(!inputs.ContainsKey(".liveness-owner.json") || Hash(File.ReadAllBytes(marker))!=(string)inputs[".liveness-owner.json"]) throw new ProtocolException("Liveness fixture ownership binding missing");
            var expected=Json.Decode(File.ReadAllBytes(marker)); Keys(expected,"token","scenario","exitCode","foreignPID");
            foreach(string key in new[]{"token","scenario","exitCode","foreignPID"}) if(!object.Equals(expected[key],testLiveness[key])) throw new ProtocolException("Liveness fixture marker/configuration differs");
        }
        void StartNative() {
            var abi=Abi(); ValidateBinding();
            var budget=Object(request,"budgetsMs"); Keys(budget,"startupMs","overallMs","cleanupMs","controllerExitMs");
            startupMs=Number(budget,"startupMs",1,30000); overallMs=Number(budget,"overallMs",1,600000); cleanupMs=Number(budget,"cleanupMs",1,10000); exitMs=Number(budget,"controllerExitMs",1,2000);
            if(overallMs<=cleanupMs+exitMs+2000) throw new ProtocolException("Insufficient overall worker budget");
            string app=String(request,"application"),cwd=String(request,"cwd"),mode=String(request,"mode");
            if(!Path.IsPathRooted(app) || !File.Exists(app) || !Path.IsPathRooted(cwd) || !Directory.Exists(cwd) || (mode!="normal" && mode!="forced-2000ms")) throw new ProtocolException("Worker launch path/mode field mismatch");
            string[] args=Arguments();
            if(request["testFault"]!=null) {
                fault=String(request,"testFault");
                if(!new[]{"assign-null-handle","resume-null-handle","query-invalid-class","close-null-handle","ready-accept-after-startup","liveness-snapshot-exit","liveness-live","liveness-signaled","liveness-foreign","liveness-trace","liveness-denied"}.Contains(fault) || args.Length<3 || Path.GetFullPath(args[0])!=Path.GetFullPath(Path.Combine(Path.GetDirectoryName(script),"owned-regression-process-tests.mjs")) || args[1]!="--actor") throw new ProtocolException("Named native fault source boundary mismatch");
                if(fault=="ready-accept-after-startup" && startupMs!=2000) throw new ProtocolException("Native late readiness control requires startup2000ms");
            }
            ValidateLivenessControl(cwd,args);
            string command=string.Join(" ",new[]{app}.Concat(args).Select(Quote)); if(command.Length+1>32767) throw new ProtocolException("Worker Unicode command bound exceeded");
            if(clock.Elapsed.TotalMilliseconds>=startupMs || stopReason!=null || protocolFailed) throw new ProtocolException("Startup expired/aborted before worker creation");
            Noninherit(Native.GetStdHandle(-10),"controller-input"); Noninherit(Native.GetStdHandle(-11),"controller-output"); Noninherit(Native.GetStdHandle(-12),"controller-errors");
            job=Own(Native.CreateJobObjectW(IntPtr.Zero,null),"job",false); Noninherit(job.Value,"job");
            var limits=new Native.ExtendedLimits(); limits.basic.flags=0x2000;
            IntPtr limitMemory=Marshal.AllocHGlobal(Size(typeof(Native.ExtendedLimits)));
            try { Marshal.StructureToPtr(limits,limitMemory,false); Check(Native.SetInformationJobObject(job.Value,9,limitMemory,(uint)Size(typeof(Native.ExtendedLimits))),"SetInformationJobObject","job"); }
            finally { Marshal.FreeHGlobal(limitMemory); Op("FreeMemory","job-limits",true,null,0); }
            var sa=new Native.SecurityAttributes { Length=(uint)Size(typeof(Native.SecurityAttributes)),Descriptor=IntPtr.Zero,Inherit=1 };
            stdin=Own(Native.CreateFileW("NUL",0x80000000,3,ref sa,3,0,IntPtr.Zero),"worker-stdin",true);
            IntPtr read,write;
            Check(Native.CreatePipe(out read,out write,ref sa,16384),"CreatePipe","worker-stdout"); stdoutRead=Own(read,"worker-stdout-read",false); stdoutWrite=Own(write,"worker-stdout-write",false); Noninherit(read,"worker-stdout-read");
            Check(Native.CreatePipe(out read,out write,ref sa,16384),"CreatePipe","worker-stderr"); stderrRead=Own(read,"worker-stderr-read",false); stderrWrite=Own(write,"worker-stderr-write",false); Noninherit(read,"worker-stderr-read");
            foreach(var inherited in new[]{stdin,stdoutWrite,stderrWrite}) { uint flags; Check(Native.GetHandleInformation(inherited.Value,out flags),"GetHandleInformation",inherited.Role); if((flags&1)==0) throw new ProtocolException("Whitelisted child handle not inheritable"); }
            UIntPtr attributeSize=UIntPtr.Zero; bool sized=Native.InitializeProcThreadAttributeList(IntPtr.Zero,1,0,ref attributeSize); int sizeError=Marshal.GetLastWin32Error();
            if(sized || sizeError!=122 || attributeSize.ToUInt64()==0 || attributeSize.ToUInt64()>65536) throw new ProtocolException("Attribute-list sizing contract failed");
            Op("InitializeAttributeSize","attributes",true,attributeSize.ToUInt64(),0);
            attributeList=Marshal.AllocHGlobal(checked((int)attributeSize.ToUInt64())); Check(Native.InitializeProcThreadAttributeList(attributeList,1,0,ref attributeSize),"InitializeProcThreadAttributeList","attributes"); attributeInitialized=true;
            handleList=Marshal.AllocHGlobal(3*IntPtr.Size); Marshal.WriteIntPtr(handleList,0,stdin.Value); Marshal.WriteIntPtr(handleList,8,stdoutWrite.Value); Marshal.WriteIntPtr(handleList,16,stderrWrite.Value);
            Check(Native.UpdateProcThreadAttribute(attributeList,0,new UIntPtr(0x00020002),handleList,new UIntPtr(24),IntPtr.Zero,IntPtr.Zero),"UpdateProcThreadAttribute","three-child-handles");
            if(request["readiness"]!=null) { readiness=new ReadyPipe(this,Object(request,"readiness")); readiness.Start(); }
            else if(mode=="forced-2000ms") throw new ProtocolException("Forced timeout readiness absent");
            EnvironmentBlock();
            var si=new Native.StartupInfoEx(); si.startup.cb=(uint)Size(typeof(Native.StartupInfoEx)); si.startup.flags=0x100; si.startup.input=stdin.Value; si.startup.output=stdoutWrite.Value; si.startup.error=stderrWrite.Value; si.attributes=attributeList;
            Native.ProcessInformation pi;
            bool launched=Native.CreateProcessW(app,new StringBuilder(command,32767),IntPtr.Zero,IntPtr.Zero,true,0x08000000|0x00080000|0x00000400|0x00000004,environment,cwd,ref si,out pi);
            int launchError=launched?0:Marshal.GetLastWin32Error();
            if(launched) { process.Value=pi.process; thread.Value=pi.thread; pid=pi.pid; created=true; }
            Op("CreateProcessW","suspended-worker",launched,launched,launchError);
            if(!launched) throw new Win32Exception(launchError,"CreateProcessW failed; native error "+launchError);
            Noninherit(process.Value,"root-process"); Noninherit(thread.Value,"root-thread");
            Native.FileTime createdTime,exitedTime,kernelTime,userTime; Check(Native.GetProcessTimes(process.Value,out createdTime,out exitedTime,out kernelTime,out userTime),"GetProcessTimes","root-process");
            Check(Native.AssignProcessToJobObject(fault=="assign-null-handle"?IntPtr.Zero:job.Value,process.Value),"AssignProcessToJobObject","suspended-worker"); assigned=true;
            bool contained; Check(Native.IsProcessInJob(process.Value,job.Value,out contained),"IsProcessInJob","root-process"); if(!contained) throw new ProtocolException("Assigned root not in owned job");
            if(!stdoutWrite.Close() || !stderrWrite.Close()) throw new ProtocolException("Worker write-copy closure failed before resume");
            if(stopReason!=null || protocolFailed) throw new ProtocolException("Worker resume aborted before execution");
            uint previous=Native.ResumeThread(fault=="resume-null-handle"?IntPtr.Zero:thread.Value); int resumeError=previous==uint.MaxValue?Marshal.GetLastWin32Error():0;
            Op("ResumeThread","root-thread",previous==1,previous,resumeError);
            if(previous!=1) throw new Win32Exception(resumeError,"ResumeThread failed/unexpected suspend count; native error "+resumeError);
            resumed=true; ownedAt=clock.Elapsed.TotalMilliseconds;
            bool controllerContained; Check(Native.IsProcessInJob(Native.GetCurrentProcess(),IntPtr.Zero,out controllerContained),"IsProcessInJob","controller-parent-containment");
            Send("OWNED",Map("rootPid",pid,"rootCreationTime",createdTime.Hex(),"jobLimitFlags",0x2000,"handleListCount",3,"excludedHandlesNoninheritable",true,"rootInOwnedJob",contained,"controllerInJob",controllerContained,"abi",abi,"controllerElapsedMs",ownedAt));
            stdoutReader=new Thread(()=>ReadWorker(stdoutRead,true)); stdoutReader.IsBackground=true; stdoutReader.Start();
            stderrReader=new Thread(()=>ReadWorker(stderrRead,false)); stderrReader.IsBackground=true; stderrReader.Start();
        }
        void ReadWorker(Handle pipe,bool stdout) {
            IntPtr memory=Marshal.AllocHGlobal(16384);
            try {
                IntPtr duplicate; Check(Native.DuplicateHandle(Native.GetCurrentProcess(),Native.GetCurrentThread(),Native.GetCurrentProcess(),out duplicate,0,false,2),"DuplicateHandle",stdout?"stdout-reader-thread":"stderr-reader-thread");
                var held=Own(duplicate,stdout?"stdout-reader-thread":"stderr-reader-thread",false); Noninherit(duplicate,held.Role);
                lock(gate) { if(stdout) stdoutThread=held; else stderrThread=held; }
                while(true) {
                    uint got; bool ok=Native.ReadFile(pipe.Value,memory,16384,out got,IntPtr.Zero); int error=ok?0:Marshal.GetLastWin32Error();
                    if(!ok && error==109 || ok && got==0) {
                        Op("ReadFileEOF",pipe.Role,true,error,0);
                        lock(gate) { if(stdout) stdoutEof=true; else stderrEof=true; } break;
                    }
                    if(!ok) { Error(cleanupErrors,"ReadFile","Worker stream read failed; native error "+error,error,null); break; }
                    byte[] bytes=new byte[got]; Marshal.Copy(memory,bytes,0,(int)got);
                    lock(gate) {
                        if(stdout) stdoutBytes+=got; else stderrBytes+=got;
                        if(stdoutBytes+stderrBytes>8388608) { outputOverflow=true; if(stopReason==null) stopReason="output-overflow"; continue; }
                    }
                    Send("DATA",Map("stream",stdout?"stdout":"stderr","bytesBase64",Convert.ToBase64String(bytes)));
                }
            } catch(Exception error) { Error(cleanupErrors,"ReadFile","Worker reader failed",error is Win32Exception?(int?)((Win32Exception)error).NativeErrorCode:null,error); }
            finally { Marshal.FreeHGlobal(memory); }
        }
        bool RootExited(bool emit) {
            if(!created || process.Value==IntPtr.Zero) return false;
            if(!rootSignaled) {
                uint wait=Native.WaitForSingleObject(process.Value,0); int error=wait==uint.MaxValue?Marshal.GetLastWin32Error():0;
                rootWaitResult=wait; rootWaitError=wait==uint.MaxValue?(int?)error:null;
                if(wait==258) return false;
                Op("WaitForSingleObject","root-process",wait==0,wait,error);
                if(wait!=0) throw new Win32Exception(error,"Native root wait failed; error "+error);
                rootSignaled=true;
            }
            if(!hasRawExit) { Check(Native.GetExitCodeProcess(process.Value,out rawExit),"GetExitCodeProcess","root-process"); hasRawExit=true; }
            if(emit && !rootReported) { rootReported=true; Send("ROOT_EXIT",Map("rawWorkerExitDWORD",rawExit,"nativeRootWait",Map("result",0,"signaled",true,"win32Error",null),"controllerElapsedMs",clock.Elapsed.TotalMilliseconds)); }
            return true;
        }
        uint Accounting() {
            IntPtr memory=Marshal.AllocHGlobal(Size(typeof(Native.Accounting))); uint returned;
            try {
                bool ok=Native.QueryInformationJobObject(job.Value,1,memory,(uint)Size(typeof(Native.Accounting)),out returned); int error=ok?0:Marshal.GetLastWin32Error();
                if(!ok) { Op("QueryInformationJobObject","job-accounting",false,null,error); throw new Win32Exception(error,"Job accounting query failed; native error "+error); }
                if(returned!=Size(typeof(Native.Accounting))) throw new ProtocolException("Job accounting returned size mismatch");
                uint value=((Native.Accounting)Marshal.PtrToStructure(memory,typeof(Native.Accounting))).active;
                Op("QueryInformationJobObject","job-accounting",true,value,0); return value;
            }
            finally { Marshal.FreeHGlobal(memory); }
        }
        sealed class ProcessSnapshot {
            public uint Assigned,ReturnedBytes;
            public uint[] Ids;
        }
        static string EvaluateCandidate(bool valid,bool opened,int openError,bool membershipOk,bool contained,uint? wait) {
            if(!valid) return "failure";
            if(!opened) return openError==87?"resample":"failure";
            if(!membershipOk) return "failure";
            if(!contained) return "resample";
            if(wait==0) return "exited";
            if(wait==258) return "live";
            return "failure";
        }
        static string EvaluateEmptyProof(bool rootDone,bool rawKnown,bool complete,int nonRoot,bool accountingOk,uint activeCount,bool probesClosed) {
            if(!rootDone || !rawKnown || !complete || !accountingOk || !probesClosed) return "failure";
            return nonRoot==0 && activeCount==0?"empty":"resample";
        }
        ProcessSnapshot Snapshot(string role) {
            IntPtr memory=Marshal.AllocHGlobal(4096); uint returned;
            try {
                bool ok=Native.QueryInformationJobObject(job.Value,3,memory,4096,out returned); int error=ok?0:Marshal.GetLastWin32Error();
                Op("QueryInformationJobObject",role,ok,returned,error);
                if(!ok) throw new Win32Exception(error,"Owned process-list query failed; native error "+error);
                uint assigned=unchecked((uint)Marshal.ReadInt32(memory,0)),listed=unchecked((uint)Marshal.ReadInt32(memory,4));
                if(returned<8 || returned>4096 || listed>511 || assigned!=listed || returned<8+listed*8) throw new ProtocolException("Incomplete or invalid owned process-list snapshot");
                var ids=new List<uint>(); var unique=new HashSet<uint>();
                for(int index=0;index<listed;++index) {
                    ulong value=unchecked((ulong)Marshal.ReadInt64(memory,8+index*8));
                    if(value==0 || value>uint.MaxValue || !unique.Add((uint)value)) throw new ProtocolException("Invalid or duplicate owned process-list identity");
                    ids.Add((uint)value);
                }
                Op("ProcessListSnapshot",role,true,Map("assigned",assigned,"listed",listed,"returnedBytes",returned,"pids",ids.ToArray()),0);
                return new ProcessSnapshot { Assigned=assigned,ReturnedBytes=returned,Ids=ids.ToArray() };
            } finally { Marshal.FreeHGlobal(memory); }
        }
        string Probe(uint candidate) {
            const uint access=0x00101000;
            var evidence=Map("pid",candidate,"access",access,"handleCreated",false);
            var descendant=ReserveHandle("descendant-liveness"); Exception probeFailure=null;
            try {
                // The ledger slot exists before acquisition; no allocating handoff follows it.
                descendant.Value=Native.OpenProcess(access,false,candidate); int error=descendant.Value==IntPtr.Zero?Marshal.GetLastWin32Error():0;
                evidence["handleCreated"]=descendant.Value!=IntPtr.Zero;
                Op("OpenProcess","descendant-liveness",descendant.Value!=IntPtr.Zero,evidence,error);
                if(descendant.Value==IntPtr.Zero) {
                    string missing=EvaluateCandidate(candidate!=0 && candidate!=pid,false,error,false,false,null);
                    if(missing=="failure") throw new Win32Exception(error,"Owned descendant open failed; native error "+error);
                    return missing;
                }
                bool contained; bool queried=Native.IsProcessInJob(descendant.Value,job.Value,out contained); error=queried?0:Marshal.GetLastWin32Error();
                Op("IsProcessInJob","descendant-liveness",queried,Map("pid",candidate,"contained",contained),error);
                if(!queried) throw new Win32Exception(error,"Owned descendant membership query failed; native error "+error);
                if(!contained) return EvaluateCandidate(true,true,0,true,false,null);
                uint wait=Native.WaitForSingleObject(descendant.Value,0); error=wait==uint.MaxValue?Marshal.GetLastWin32Error():0;
                Op("WaitForSingleObject","descendant-liveness",wait==0 || wait==258,wait,error);
                if(wait==258) {
                    uint published; Check(Native.GetExitCodeProcess(descendant.Value,out published),"GetExitCodeProcess","descendant-exit-status");
                    Op("PublishedExitStatus","descendant-exit-status",true,published,0);
                    if(published!=259) {
                        uint remaining=(uint)Math.Max(0,Math.Min(100,normalCleanupOrigin+cleanupMs-clock.Elapsed.TotalMilliseconds));
                        wait=Native.WaitForSingleObject(descendant.Value,remaining); error=wait==uint.MaxValue?Marshal.GetLastWin32Error():0;
                        Op("WaitForSingleObject","descendant-exit-finalization",wait==0 || wait==258,wait,error);
                    }
                }
                string decision=EvaluateCandidate(true,true,0,true,true,wait);
                if(decision=="failure") throw new Win32Exception(error,"Owned descendant wait failed; native error "+error);
                return decision;
            } catch(Exception caught) { probeFailure=caught; throw; }
            finally {
                try { if(!descendant.Close()) throw new ProtocolException("Owned descendant probe close failed"); }
                catch(Exception closeFailure) { if(probeFailure!=null) throw new AggregateException(probeFailure,closeFailure); throw; }
            }
        }
        Handle testHeld;
        IntPtr testOriginalSecurity; bool testSecuritySaved;
        string TestPath(string suffix) { return Path.Combine(String(request,"cwd"),String(testLiveness,"token")+suffix); }
        void ReleaseFixtureChild() { File.WriteAllText(TestPath(".release-child"),String(testLiveness,"token"),new UTF8Encoding(false)); }
        void PrepareLivenessBarrier(ProcessSnapshot snapshot,double deadline) {
            if(testLiveness==null || fault=="liveness-foreign" || fault=="liveness-trace") return;
            uint child=snapshot.Ids.FirstOrDefault(value=>value!=pid); if(child==0) throw new ProtocolException("Native liveness fixture child not present in snapshot");
            uint rights=fault=="liveness-denied"?0x00161000u:0x00101000u;
            testHeld=ReserveHandle("test-liveness-preliminary");
            testHeld.Value=Native.OpenProcess(rights,false,child); int openError=testHeld.Value==IntPtr.Zero?Marshal.GetLastWin32Error():0;
            Op("CreateHandle","test-liveness-preliminary",testHeld.Value!=IntPtr.Zero,null,openError);
            if(testHeld.Value==IntPtr.Zero) throw new Win32Exception(openError,"Create handle failed: test-liveness-preliminary; native error "+openError);
            bool contained; Check(Native.IsProcessInJob(testHeld.Value,job.Value,out contained),"IsProcessInJob","test-liveness-preliminary");
            if(!contained) throw new ProtocolException("Liveness preliminary candidate is not in the retained owned job");
            uint initial=Native.WaitForSingleObject(testHeld.Value,0); Op("WaitForSingleObject","test-liveness-preliminary",initial==258,initial,initial==uint.MaxValue?Marshal.GetLastWin32Error():0);
            if(!contained || initial!=258) throw new ProtocolException("Liveness barrier requires an actual live owned child");
            Native.FileTime creation,exit,kernel,user; Check(Native.GetProcessTimes(testHeld.Value,out creation,out exit,out kernel,out user),"GetProcessTimes","test-liveness-preliminary");
            Op("TestLivenessSnapshotBarrier","source-bound-liveness",true,Map("origin","native-barrier","pid",child,"creationTime",creation.Hex(),"scenario",fault),0);
            if(fault=="liveness-live") { if(!testHeld.Close()) throw new ProtocolException("Live fixture probe close failed"); testHeld=null; return; }
            if(fault=="liveness-denied") { DenyFixtureProbe(); return; }
            ReleaseFixtureChild();
            if(fault=="liveness-snapshot-exit") {
                if(!testHeld.Close()) throw new ProtocolException("Snapshot fixture preliminary close failed"); testHeld=null;
                while(Accounting()!=0) { if(clock.Elapsed.TotalMilliseconds>=deadline || protocolFailed || stopReason!=null) throw new ProtocolException("Liveness barrier did not observe native empty accounting"); Thread.Sleep(2); }
                Op("TestLivenessBarrierReleased","source-bound-liveness",true,Map("origin","native-barrier","pid",child,"accounting",0),0);
            } else {
                uint remaining=(uint)Math.Max(0,deadline-clock.Elapsed.TotalMilliseconds);
                uint wait=Native.WaitForSingleObject(testHeld.Value,remaining); Op("WaitForSingleObject","test-signaled-descendant",wait==0,wait,wait==uint.MaxValue?Marshal.GetLastWin32Error():0);
                if(wait!=0) throw new ProtocolException("Liveness signaled fixture did not exit");
                uint code; Check(Native.GetExitCodeProcess(testHeld.Value,out code),"GetExitCodeProcess","test-signaled-descendant");
                Op("TestSignaledDescendantExit","source-bound-liveness",true,code,0);
            }
        }
        void DenyFixtureProbe() {
            uint needed; bool sized=Native.GetKernelObjectSecurity(testHeld.Value,4,IntPtr.Zero,0,out needed); int error=Marshal.GetLastWin32Error();
            Op("GetKernelObjectSecurity","test-owned-descendant-dacl-size",sized,needed,error);
            if(sized || error!=122 || needed==0 || needed>65536) throw new ProtocolException("Fixture-only DACL sizing failed");
            testOriginalSecurity=Marshal.AllocHGlobal((int)needed);
            Check(Native.GetKernelObjectSecurity(testHeld.Value,4,testOriginalSecurity,needed,out needed),"GetKernelObjectSecurity","test-owned-descendant-dacl");
            testSecuritySaved=true;
            IntPtr denied=IntPtr.Zero; uint bytes; Exception daclFailure=null;
            try {
                string sid=WindowsIdentity.GetCurrent().User.Value;
                Check(Native.ConvertStringSecurityDescriptorToSecurityDescriptorW("D:(D;;0x00101000;;;"+sid+")(A;;GA;;;SY)(A;;GA;;;"+sid+")",1,out denied,out bytes),"ConvertStringSecurityDescriptorToSecurityDescriptorW","test-owned-descendant-dacl");
                if(bytes==0 || bytes>65536) throw new ProtocolException("Fixture-only DACL descriptor bound exceeded");
                Check(Native.SetKernelObjectSecurity(testHeld.Value,4,denied),"SetKernelObjectSecurity","test-owned-descendant-dacl");
            } catch(Exception caught) { daclFailure=caught; throw; }
            finally {
                try {
                    if(denied!=IntPtr.Zero) {
                        IntPtr remaining=Native.LocalFree(denied); int freeError=remaining==IntPtr.Zero?0:Marshal.GetLastWin32Error();
                        if(remaining!=IntPtr.Zero) Error(cleanupErrors,"LocalFree","Fixture descriptor release failed",freeError,null);
                        Op("LocalFree","test-owned-descendant-dacl",remaining==IntPtr.Zero,remaining.ToInt64(),freeError);
                    }
                } catch(Exception cleanupFailure) { if(daclFailure!=null) throw new AggregateException(daclFailure,cleanupFailure); throw; }
            }
        }
        void CloseTestProbe() {
            Exception restoreFailure=null;
            if(testOriginalSecurity!=IntPtr.Zero) {
                try { if(testSecuritySaved) Check(Native.SetKernelObjectSecurity(testHeld.Value,4,testOriginalSecurity),"SetKernelObjectSecurity","test-owned-descendant-dacl-restore"); }
                catch(Exception error) { restoreFailure=error; }
                finally { Marshal.FreeHGlobal(testOriginalSecurity); testOriginalSecurity=IntPtr.Zero; testSecuritySaved=false; }
            }
            if(testHeld!=null) {
                bool closed=testHeld.Close(); testHeld=null;
                if(!closed) {
                    var closeFailure=new ProtocolException("Fixture preliminary handle close failed");
                    if(restoreFailure!=null) throw new AggregateException(restoreFailure,closeFailure);
                    throw closeFailure;
                }
            }
            if(restoreFailure!=null) throw restoreFailure;
        }
        void RunInjectedLivenessTrace() {
            var vectors=new Dictionary<string,string> {
                {"invalid-pid",EvaluateCandidate(false,false,87,false,false,null)},
                {"unavailable87",EvaluateCandidate(true,false,87,false,false,null)},
                {"access-denied5",EvaluateCandidate(true,false,5,false,false,null)},
                {"membership-failed",EvaluateCandidate(true,true,0,false,false,null)},
                {"foreign-reused",EvaluateCandidate(true,true,0,true,false,null)},
                {"owned-live-reused",EvaluateCandidate(true,true,0,true,true,258)},
                {"owned-signaled259",EvaluateCandidate(true,true,0,true,true,0)},
                {"wait-failed",EvaluateCandidate(true,true,0,true,true,uint.MaxValue)},
                {"root-unsignaled",EvaluateEmptyProof(false,true,true,0,true,0,true)},
                {"incomplete-snapshot",EvaluateEmptyProof(true,true,false,0,true,0,true)},
                {"root-only-active",EvaluateEmptyProof(true,true,true,0,true,1,true)},
                {"unaccounted-list",EvaluateEmptyProof(true,true,true,1,true,0,true)},
                {"accounting-failed",EvaluateEmptyProof(true,true,true,0,false,0,true)},
                {"close-failed",EvaluateEmptyProof(true,true,true,0,true,0,false)},
                {"empty-shaped",EvaluateEmptyProof(true,true,true,0,true,0,true)}
            };
            Op("InjectedLivenessTrace","source-bound-liveness",true,Map("origin","injected-trace","nativeCalls",0,"vectors",vectors),0);
        }
        bool HasDescendant() {
            double cleanupDeadline=Math.Min(normalCleanupOrigin+cleanupMs,overallMs-exitMs),refreshDeadline=-1;
            bool first=true;
            while(true) {
                if(!rootSignaled || !hasRawExit || !RootExited(false)) throw new ProtocolException("Liveness classification requires retained root exit");
                if(clock.Elapsed.TotalMilliseconds>=cleanupDeadline || protocolFailed || stopReason!=null) throw new ProtocolException("Liveness reconciliation stopped before empty proof");
                lock(gate) { if(operations.Count>=3968) throw new ProtocolException("Liveness diagnostics leave no checked-cleanup operation capacity"); }
                var snapshot=Snapshot("job-process-list");
                Exception livenessFailure=null;
                try {
                    if(first) {
                        PrepareLivenessBarrier(snapshot,cleanupDeadline);
                        if(fault=="liveness-trace") RunInjectedLivenessTrace();
                        if(fault=="liveness-foreign") {
                            uint foreign=Convert.ToUInt32(testLiveness["foreignPID"]);
                            Op("InjectedSnapshotCandidate","source-bound-liveness",true,Map("origin","injected-candidate","pid",foreign),0);
                            if(Probe(foreign)!="resample") throw new ProtocolException("Foreign fixture candidate was not rejected by actual membership");
                        }
                    }
                    foreach(uint candidate in snapshot.Ids) {
                        if(candidate==pid) continue;
                        lock(gate) { if(operations.Count>=3968) throw new ProtocolException("Liveness diagnostics leave no checked-cleanup operation capacity"); }
                        string decision=Probe(candidate);
                        Op("LivenessCandidateDecision","descendant-liveness",true,Map("pid",candidate,"decision",decision,"origin","native"),0);
                        if(decision=="live") return true;
                    }
                } catch(Exception caught) { livenessFailure=caught; throw; }
                finally {
                    try { CloseTestProbe(); }
                    catch(Exception closeFailure) { if(livenessFailure!=null) throw new AggregateException(livenessFailure,closeFailure); throw; }
                }
                if(clock.Elapsed.TotalMilliseconds>=cleanupDeadline || protocolFailed || stopReason!=null) throw new ProtocolException("Liveness stopped before fresh native empty evidence");
                var fresh=Snapshot("preclassification-process-list");
                uint count=Accounting();
                int nonRoot=fresh.Ids.Count(value=>value!=pid);
                string empty=EvaluateEmptyProof(rootSignaled,hasRawExit,true,nonRoot,true,count,allHandlesClosed);
                Op("LivenessEmptyProof","preclassification-owned-job",true,Map("origin","native","rootSignaled",rootSignaled,"rawExitKnown",hasRawExit,"nonRoot",nonRoot,"active",count,"probesClosed",allHandlesClosed,"decision",empty),0);
                if(empty=="empty") return false;
                if(empty=="failure") throw new ProtocolException("Native empty proof failed");
                if(refreshDeadline<0) refreshDeadline=Math.Min(cleanupDeadline,clock.Elapsed.TotalMilliseconds+100);
                if(clock.Elapsed.TotalMilliseconds>=refreshDeadline) throw new ProtocolException("Native list/accounting remained unresolved within liveness bound");
                first=false; Thread.Sleep(2);
            }
        }

        void RunWorker() {
            string mode=String(request,"mode");
            while(true) {
                if(protocolFailed) { kind="protocol-failure"; break; }
                if(outputOverflow) { kind="output-overflow"; break; }
                string reason; lock(gate) reason=stopReason;
                if(peer!=null && mode=="forced-2000ms" && reason==null) {
                    uint wait=Native.WaitForSingleObject(peer.Value,0); int error=wait==uint.MaxValue?Marshal.GetLastWin32Error():0;
                    if(wait==0) { Op("WaitForSingleObject","readiness-peer-premature-exit",true,wait,0); kind="setup-failure"; Error(primary,kind,"Verified descendant exited before the forced interval completed",null,null); break; }
                    if(wait!=258) { Op("WaitForSingleObject","readiness-peer-monitor",false,wait,error); throw new Win32Exception(error,"Readiness peer monitor failed; native error "+error); }
                }
                if(reason!=null) {
                    kind=reason=="forced-2000ms"?"forced-timeout":reason=="startup-deadline"?"setup-failure":reason=="output-overflow"?"output-overflow":reason=="parent-abort"?"controller-failure":"execution-timeout"; break;
                }
                if(readiness!=null && readyAt<0) {
                    if(clock.Elapsed.TotalMilliseconds>=startupMs) { kind="setup-failure"; Error(primary,kind,"Descendant readiness startup deadline exceeded before polling",null,null); break; }
                    if(readiness.Poll()) readyAt=clock.Elapsed.TotalMilliseconds;
                }
                if(RootExited(true)) {
                    if(mode=="normal") { normalCleanupOrigin=clock.Elapsed.TotalMilliseconds; if(HasDescendant()) kind="surviving-descendant"; break; }
                    if(readyAt<0 && Accounting()==0) { kind="setup-failure"; Error(primary,kind,"Root exited before verified descendant readiness",null,null); break; }
                }
                double elapsed=clock.Elapsed.TotalMilliseconds;
                if(mode=="forced-2000ms" && readyAt<0 && elapsed>=startupMs) { kind="setup-failure"; Error(primary,kind,"Descendant readiness startup deadline exceeded",null,null); break; }
                if(elapsed>=overallMs-cleanupMs-exitMs) { kind="execution-timeout"; break; }
                Thread.Sleep(2);
            }
        }
        bool Release(double deadline) {
            bool terminated=true; stopAt=clock.Elapsed.TotalMilliseconds;
            if(job!=null && assigned) terminated=CleanupBool(Native.TerminateJobObject(job.Value,1),"TerminateJobObject","owned-job");
            if(created && !resumed) terminated=CleanupBool(Native.TerminateProcess(process.Value,1),"TerminateProcess","held-suspended-root") && terminated;
            if(stdoutWrite!=null) stdoutWrite.Close(); if(stderrWrite!=null) stderrWrite.Close();
            bool zero=job==null,rootDone=!created;
            while(clock.Elapsed.TotalMilliseconds<deadline) {
                try { if(created) rootDone=RootExited(false); if(job!=null) { active=Accounting(); zero=active==0; } }
                catch(Exception error) { Error(cleanupErrors,"NativeQuery","Native completion query failed",error is Win32Exception?(int?)((Win32Exception)error).NativeErrorCode:null,error); break; }
                if(rootDone && zero) break; Thread.Sleep(2);
            }
            if(fault=="query-invalid-class") {
                IntPtr info=Marshal.AllocHGlobal(Size(typeof(Native.Accounting))); uint returned;
                // NULL selects the caller's current job. Use our retained job
                // with an unsupported information class as the real error oracle.
                try { CleanupBool(Native.QueryInformationJobObject(job.Value,uint.MaxValue,info,(uint)Size(typeof(Native.Accounting)),out returned),"QueryInformationJobObject","test-invalid-class-owned-job"); }
                finally { Marshal.FreeHGlobal(info); }
            }
            if(readiness!=null) readiness.Stop(deadline);
            if(peer!=null) {
                // Job accounting can reach zero before the last process object
                // signals. The retained peer handle needs its own bounded wait.
                uint remaining=(uint)Math.Max(0,Math.Min(int.MaxValue,deadline-clock.Elapsed.TotalMilliseconds));
                uint wait=Native.WaitForSingleObject(peer.Value,remaining); int error=wait==uint.MaxValue?Marshal.GetLastWin32Error():0;
                Op("WaitForSingleObject","verified-peer",wait==0,wait,error); peerSignaled=wait==0;
                if(!peerSignaled) Error(cleanupErrors,"WaitForSingleObject","Verified peer wait did not signal",error,null);
            }
            // Even a never-resumed suspended root must produce real stream EOF;
            // it is not inferred from root exit or absence of application code.
            if(stdoutReader==null && stdoutRead!=null) { stdoutReader=new Thread(()=>ReadWorker(stdoutRead,true)); stdoutReader.IsBackground=true; stdoutReader.Start(); }
            if(stderrReader==null && stderrRead!=null) { stderrReader=new Thread(()=>ReadWorker(stderrRead,false)); stderrReader.IsBackground=true; stderrReader.Start(); }
            foreach(var io in new[]{stdoutReader,stderrReader}) if(io!=null) {
                int remaining=(int)Math.Max(0,Math.Min(500,deadline-clock.Elapsed.TotalMilliseconds));
                if(!io.Join(remaining)) {
                    Handle held=io==stdoutReader?stdoutThread:stderrThread;
                    if(held!=null) { bool canceled=Native.CancelSynchronousIo(held.Value); int error=canceled?0:Marshal.GetLastWin32Error(); Op("CancelSynchronousIo",held.Role,canceled || error==1168,canceled,error); }
                    if(!io.Join((int)Math.Max(0,Math.Min(500,deadline-clock.Elapsed.TotalMilliseconds)))) { allHandlesClosed=false; Error(cleanupErrors,"ReaderJoin","Worker reader deadline exceeded",null,null); }
                }
            }
            if(!created) { stdoutEof=true; stderrEof=true; }
            if(fault=="close-null-handle") CleanupBool(Native.CloseHandle(IntPtr.Zero),"CloseHandle","test-null-handle");
            FreeAllocations();
            Handle[] owned; lock(gate) owned=handles.ToArray();
            foreach(var handle in owned.Reverse()) if(handle!=job) handle.Close();
            if(job!=null) job.Close();
            cleanupFinishedAt=clock.Elapsed.TotalMilliseconds;
            return terminated && zero && rootDone && (peer==null || peerSignaled) && stdoutEof && stderrEof && allHandlesClosed && cleanupErrors.Count==0 && cleanupFinishedAt<=deadline && !protocolFailed;
        }
        void FreeAllocations() {
            try { if(attributeInitialized) { Native.DeleteProcThreadAttributeList(attributeList); attributeInitialized=false; Op("DeleteProcThreadAttributeList","attributes",true,null,0); } }
            catch(Exception error) { Error(cleanupErrors,"DeleteProcThreadAttributeList","Attribute release failed",null,error); }
            foreach(var allocation in new[]{new KeyValuePair<string,IntPtr>("attributes",attributeList),new KeyValuePair<string,IntPtr>("handle-list",handleList),new KeyValuePair<string,IntPtr>("worker-environment",environment)}) {
                if(allocation.Value!=IntPtr.Zero) try { Marshal.FreeHGlobal(allocation.Value); Op("FreeMemory",allocation.Key,true,null,0); } catch(Exception error) { Error(cleanupErrors,"FreeMemory","Owned memory release failed",null,error); }
            }
            attributeList=IntPtr.Zero; handleList=IntPtr.Zero; environment=IntPtr.Zero;
        }
        sealed class ReadyPipe {
            readonly Owner owner; readonly Dictionary<string,object> config;
            Handle pipe,eventHandle; IntPtr overlap,memory; bool pending,connected,reading,complete; readonly MemoryStream received=new MemoryStream();
            public ReadyPipe(Owner parent,Dictionary<string,object> ready) { owner=parent; config=ready; }
            public void Start() {
                Keys(config,"pipeName","nonce","generation","sourceScriptSha256","sourceScriptPath");
                if(String(config,"pipeName")!="\\\\.\\pipe\\nstu-owned-"+owner.runId+"-"+owner.generation || String(config,"nonce")!=owner.nonce || String(config,"generation")!=owner.generation || Hash(File.ReadAllBytes(String(config,"sourceScriptPath")))!=String(config,"sourceScriptSha256")) throw new ProtocolException("Readiness source/identity mismatch");
                IntPtr descriptor; uint size;
                using(var identity=WindowsIdentity.GetCurrent()) owner.Check(Native.ConvertStringSecurityDescriptorToSecurityDescriptorW("D:P(A;;GA;;;"+identity.User.Value+")",1,out descriptor,out size),"ConvertStringSecurityDescriptor","readiness-current-user");
                try {
                    var sa=new Native.SecurityAttributes { Length=(uint)Size(typeof(Native.SecurityAttributes)),Descriptor=descriptor,Inherit=0 };
                    // Node's named-pipe client opens a duplex endpoint. Control
                    // readiness still reads only the bounded typed challenge.
                    pipe=owner.Own(Native.CreateNamedPipeW(String(config,"pipeName"),0x40000003|0x00080000,8,1,4096,4096,0,ref sa),"readiness-pipe",true); owner.Noninherit(pipe.Value,pipe.Role);
                } finally {
                    IntPtr remaining=Native.LocalFree(descriptor); int error=remaining==IntPtr.Zero?0:Marshal.GetLastWin32Error(); owner.Op("LocalFree","readiness-descriptor",remaining==IntPtr.Zero,null,error); if(remaining!=IntPtr.Zero) owner.Error(owner.cleanupErrors,"LocalFree","Readiness descriptor release failed",error,null);
                }
                eventHandle=owner.Own(Native.CreateEventW(IntPtr.Zero,true,false,null),"readiness-event",false); owner.Noninherit(eventHandle.Value,eventHandle.Role);
                overlap=Marshal.AllocHGlobal(Size(typeof(Native.Overlapped))); memory=Marshal.AllocHGlobal(4096); Reset();
                bool ok=Native.ConnectNamedPipe(pipe.Value,overlap); int connectError=ok?0:Marshal.GetLastWin32Error();
                owner.Op("ConnectNamedPipe",pipe.Role,ok || connectError==997 || connectError==535,ok?0:connectError,connectError);
                if(ok || connectError==535) connected=true; else if(connectError==997) pending=true; else throw new Win32Exception(connectError,"Readiness connection failed; native error "+connectError);
            }
            void Reset() { owner.Check(Native.ResetEvent(eventHandle.Value),"ResetEvent",eventHandle.Role); Marshal.StructureToPtr(new Native.Overlapped { eventHandle=eventHandle.Value },overlap,false); }
            void BeginRead() {
                Reset(); reading=true; uint got;
                bool ok=Native.ReadFile(pipe.Value,memory,(uint)(4096-received.Length),out got,overlap); int error=ok?0:Marshal.GetLastWin32Error();
                if(ok) Consume(got); else if(error==997) pending=true; else throw new Win32Exception(error,"Readiness read failed; native error "+error);
            }
            void Consume(uint got) {
                if(got==0) throw new ProtocolException("Readiness peer EOF before challenge");
                byte[] bytes=new byte[got]; Marshal.Copy(memory,bytes,0,(int)got); received.Write(bytes,0,bytes.Length);
                if(received.Length<4) return; byte[] all=received.ToArray(); uint length=BitConverter.ToUInt32(all,0);
                if(length==0 || length>4092 || all.Length>length+4) throw new ProtocolException("Readiness frame bound mismatch");
                if(all.Length<length+4) return;
                var frame=Json.Decode(all.Skip(4).ToArray()); Keys(frame,"v","type","runId","nonce","generation","sourceScriptSha256");
                if(Number(frame,"v",1,1)!=1 || String(frame,"type")!="READY" || String(frame,"runId")!=owner.runId || String(frame,"nonce")!=owner.nonce || String(frame,"generation")!=owner.generation || String(frame,"sourceScriptSha256")!=String(config,"sourceScriptSha256")) throw new ProtocolException("Readiness challenge/source mismatch");
                uint pid; owner.Check(Native.GetNamedPipeClientProcessId(pipe.Value,out pid),"GetNamedPipeClientProcessId",pipe.Role);
                if(pid==owner.pid) throw new ProtocolException("Readiness peer is the launched root, not a descendant");
                var candidate=owner.Own(Native.OpenProcess(0x00101000,false,pid),"readiness-candidate",false); bool verified=false;
                try {
                    owner.Noninherit(candidate.Value,candidate.Role);
                    bool contained; owner.Check(Native.IsProcessInJob(candidate.Value,owner.job.Value,out contained),"IsProcessInJob","readiness-peer");
                    uint wait=Native.WaitForSingleObject(candidate.Value,0); int error=wait==uint.MaxValue?Marshal.GetLastWin32Error():0; owner.Op("WaitForSingleObject","readiness-peer-live",wait==258,wait,error);
                    if(!contained || wait!=258) throw new ProtocolException("Readiness peer is not live in owned job");
                    Native.FileTime created,exited,kernel,user; owner.Check(Native.GetProcessTimes(candidate.Value,out created,out exited,out kernel,out user),"GetProcessTimes","readiness-peer");
                    if(owner.fault=="ready-accept-after-startup") {
                        owner.Op("TestReadyAcceptanceDelay","source-bound-native-ready",true,owner.clock.Elapsed.TotalMilliseconds,0);
                        while(owner.clock.Elapsed.TotalMilliseconds<2050) Thread.Sleep((int)Math.Max(1,Math.Min(5,2050-owner.clock.Elapsed.TotalMilliseconds)));
                        owner.Op("TestReadyAcceptanceReleased","source-bound-native-ready",true,owner.clock.Elapsed.TotalMilliseconds,0);
                    }
                    // Rejected peers are temporary query handles, never later
                    // process-wait or termination targets of this owner.
                    var readyPayload=Map("peerPid",pid,"peerCreationTime",created.Hex(),"peerInOwnedJob",contained,"peerWaitResult",wait,"sourceScriptSha256",String(config,"sourceScriptSha256"),"controllerElapsedMs",owner.clock.Elapsed.TotalMilliseconds);
                    lock(owner.gate) {
                        if(owner.clock.Elapsed.TotalMilliseconds>=owner.startupMs) throw new ProtocolException("Readiness startup deadline exceeded before acceptance");
                        if(owner.stopReason!=null || owner.protocolFailed) return;
                        owner.peer=candidate; verified=true;
                        owner.Send("READY",readyPayload); complete=true;
                    }
                } finally { if(!verified) candidate.Close(); }
            }
            public bool Poll() {
                if(complete) return true;
                if(pending) {
                    uint wait=Native.WaitForSingleObject(eventHandle.Value,0); if(wait==258) return false; owner.Check(wait==0,"WaitForSingleObject",eventHandle.Role);
                    uint got; owner.Check(Native.GetOverlappedResult(pipe.Value,overlap,out got,false),"GetOverlappedResult",pipe.Role); pending=false;
                    if(reading) Consume(got); else connected=true;
                }
                if(connected && !complete && !pending) BeginRead();
                return complete;
            }
            public void Stop(double deadline) {
                try {
                    if(pending && pipe!=null) {
                        bool canceled=Native.CancelIoEx(pipe.Value,overlap); int error=canceled?0:Marshal.GetLastWin32Error();
                        owner.Op("CancelIoEx",pipe.Role,canceled || error==1168,canceled,error);
                        if(!canceled && error!=1168) owner.Error(owner.cleanupErrors,"CancelIoEx","Readiness cancel failed",error,null);
                        uint wait=Native.WaitForSingleObject(eventHandle.Value,(uint)Math.Max(0,deadline-owner.clock.Elapsed.TotalMilliseconds));
                        if(wait==0) {
                            uint got; bool done=Native.GetOverlappedResult(pipe.Value,overlap,out got,false); int completion=done?0:Marshal.GetLastWin32Error();
                            owner.Op("GetOverlappedResult","readiness-cancel-completion",done || completion==995,completion,completion);
                            if(!done && completion!=995) owner.Error(owner.cleanupErrors,"GetOverlappedResult","Readiness cancel completion failed",completion,null);
                            pending=false;
                        } else owner.Error(owner.cleanupErrors,"WaitForSingleObject","Readiness pending operation did not complete",wait==uint.MaxValue?Marshal.GetLastWin32Error():0,null);
                    }
                    if(connected && pipe!=null) owner.CleanupBool(Native.DisconnectNamedPipe(pipe.Value),"DisconnectNamedPipe",pipe.Role);
                    if(!pending) { if(overlap!=IntPtr.Zero) Marshal.FreeHGlobal(overlap); if(memory!=IntPtr.Zero) Marshal.FreeHGlobal(memory); overlap=IntPtr.Zero; memory=IntPtr.Zero; owner.Op("FreeMemory","readiness-overlapped-buffers",true,null,0); }
                } catch(Exception error) { owner.Error(owner.cleanupErrors,"ReadinessRelease","Readiness release failed",null,error); }
            }
        }
        public int Run() {
            writer=new Thread(OutputLoop); writer.IsBackground=true; writer.Start(); reader=new Thread(InputLoop); reader.IsBackground=true; reader.Start();
            try {
                while(request==null && !protocolFailed && clock.ElapsedMilliseconds<startupMs) Thread.Sleep(2);
                if(request==null || protocolFailed) {
                    Console.Error.WriteLine(Json.Serializer().Serialize(Map("kind","controller-startup","primaryErrors",primary,"cleanupConfirmed",false,"bootstrapOutsideWorkerJob",true)));
                    return 1;
                }
                StartNative(); RunWorker();
            } catch(Exception error) {
                kind=error is Win32Exception && error.Message.StartsWith("CreateProcessW",StringComparison.Ordinal)?"worker-spawn-failure":"setup-failure";
                var aggregate=error as AggregateException;
                foreach(Exception cause in aggregate!=null?aggregate.Flatten().InnerExceptions:new[]{error}.AsEnumerable())
                    Error(primary,kind,cause.Message,cause is Win32Exception?(int?)((Win32Exception)cause).NativeErrorCode:null,cause);
            }
            if(request==null) return 1;
            double deadline=(normalCleanupOrigin>=0?normalCleanupOrigin:clock.Elapsed.TotalMilliseconds)+cleanupMs; bool confirmed=false;
            try { confirmed=Release(deadline); } catch(Exception error) { Error(cleanupErrors,"Cleanup","Owned cleanup failed",null,error); }
            bool expired=clock.Elapsed.TotalMilliseconds>deadline;
            if(expired) Error(cleanupErrors,"CleanupDeadline","Native cleanup deadline exceeded",null,null);
            confirmed=confirmed && !expired;
            if(protocolFailed) kind="protocol-failure"; if(outputOverflow) kind="output-overflow";
            if(kind=="surviving-descendant") Error(primary,kind,"Root exited while owned descendants remained",null,null);
            if(kind=="worker-exit" && hasRawExit && rawExit!=0) Error(primary,kind,"Worker native exit "+rawExit,null,null);
            terminal=true;
            var execution=Map("kind",kind,"rawWorkerExitDWORD",hasRawExit?(object)rawExit:null,"effectiveExitCode",kind=="worker-exit" && hasRawExit && rawExit==0 && confirmed && primary.Count==0?0:1,
                "nativeRootWait",Map("result",rootWaitResult.HasValue?(object)rootWaitResult.Value:null,"signaled",rootSignaled,"win32Error",rootWaitError.HasValue?(object)rootWaitError.Value:null),"primaryErrors",primary);
            var cleanup=Map("scope",assigned?"job-tree":created?"suspended-root":"no-child-created","confirmed",confirmed && !expired,"activeProcessesLast",job!=null?(object)active:null,
                "rootWaitSignaled",rootSignaled,"peerWaitSignaled",peer!=null?(object)peerSignaled:null,"handlesClosedConfirmed",allHandlesClosed,"operations",operations,"errors",cleanupErrors,"deadlineExpired",expired);
            var timing=Map("originUtc",originUtc,"ownedElapsedMs",ownedAt>=0?(object)ownedAt:null,"readyElapsedMs",readyAt>=0?(object)readyAt:null,"stopElapsedMs",stopAt>=0?(object)stopAt:null,"cleanupFinishedElapsedMs",cleanupFinishedAt>=0?(object)cleanupFinishedAt:null,"terminalElapsedMs",clock.Elapsed.TotalMilliseconds);
            Send("TERMINAL",Map("execution",execution,"cleanup",cleanup,"controllerTiming",timing,
                "output",Map("stdoutBytes",stdoutBytes,"stderrBytes",stderrBytes,"overflow",outputOverflow,"workerStdoutEOF",stdoutEof,"workerStderrEOF",stderrEof),
                "bootstrap",Map("outsideWorkerJob",true,"completed",true,"wholeControllerTreeCleanupConfirmed",false)));
            messages.CompleteAdding();
            return writerDone.WaitOne(exitMs) && !protocolFailed ? 0 : 1;
        }
    }
    public static class Entry {
        public static int Run(string file,Stopwatch clock,string originUtc) {
            try { return new Owner(file,clock,originUtc).Run(); }
            catch(Exception error) { Console.Error.WriteLine("Controller bootstrap/owner failure: "+error.ToString()); return 1; }
        }
    }
}
'@
$null = Add-Type -TypeDefinition $nativeSource -Language CSharp -ReferencedAssemblies @('System.dll', 'System.Core.dll', 'System.Web.Extensions.dll') -ErrorAction Stop -WarningAction Stop
$ownerExit = [NstuOwnedRegression.Entry]::Run($PSCommandPath, $ownerClock, $ownerOriginUtc)
exit $ownerExit

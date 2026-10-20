using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using libomtnet;
class Interop {
 const string Dll="omt";
 [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] static extern IntPtr test_create();
 [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] static extern void omt_send_destroy(IntPtr p);
 [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] static extern int omt_send(IntPtr p,ref OMTMediaFrame f);
 [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] static extern IntPtr omt_send_receive(IntPtr p,int timeout);
 [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] static extern int omt_send_gettally(IntPtr p,int timeout,ref OMTTally t);
 [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] static extern IntPtr omt_receive_create(string url,OMTFrameType types,OMTPreferredVideoFormat format,OMTReceiveFlags flags);
 [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] static extern void omt_receive_destroy(IntPtr p);
 [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] static extern IntPtr omt_receive(IntPtr p,OMTFrameType types,int timeout);
 [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] static extern void omt_receive_settally(IntPtr p,ref OMTTally t);
 [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] static extern int omt_receive_send(IntPtr p,ref OMTMediaFrame f);
 [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] static extern int omt_receive_gettally(IntPtr p,int timeout,ref OMTTally t);
 static int checks=0;
 static void Check(bool ok,string text){if(!ok)throw new Exception(text);Console.WriteLine("PASS "+text);checks++;}
 static bool Until(Func<bool> f){var end=DateTime.UtcNow.AddSeconds(6);do{if(f())return true;Thread.Sleep(10);}while(DateTime.UtcNow<end);return false;}
 static byte[] Bytes(IntPtr p,int n){var b=new byte[n];Marshal.Copy(p,b,0,n);return b;}
 static bool Equal(byte[] a,byte[] b){if(a.Length!=b.Length)return false;for(int i=0;i<a.Length;i++)if(a[i]!=b[i])return false;return true;}
 static int Main(){try{Run();Console.WriteLine("PASS official libomtnet interoperability: "+checks);return 0;}catch(Exception e){Console.Error.WriteLine(e);return 1;}}
 static void Run(){
  Environment.SetEnvironmentVariable("OMT_STORAGE_PATH",Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"settings"));
  OMTSettings.GetInstance().SetInteger("NetworkPortStart",26491);
  OMTSettings.GetInstance().SetInteger("NetworkPortEnd",26499);
  OMTSettings.GetInstance().SetString("DiscoveryServer","omt://127.0.0.1:26599");
  byte[] pixels=new byte[320*176*4];for(int i=0;i<pixels.Length;i+=4){pixels[i]=(byte)(i%127);pixels[i+1]=65;pixels[i+2]=180;pixels[i+3]=128;}
  float[] audio={1,-1,0,0};byte[] meta=System.Text.Encoding.UTF8.GetBytes("<fixture/>\0");
  var px=GCHandle.Alloc(pixels,GCHandleType.Pinned);var au=GCHandle.Alloc(audio,GCHandleType.Pinned);var mt=GCHandle.Alloc(meta,GCHandleType.Pinned);
  try {
   OMTMediaFrame video=new OMTMediaFrame{Type=OMTFrameType.Video,Codec=(int)OMTCodec.BGRA,Width=320,Height=176,Stride=1280,FrameRateN=60000,FrameRateD=1001,Flags=OMTVideoFlags.Alpha,ColorSpace=(OMTColorSpace)709,Timestamp=123456,Data=px.AddrOfPinnedObject(),DataLength=pixels.Length};
   OMTMediaFrame sound=new OMTMediaFrame{Type=OMTFrameType.Audio,Codec=(int)OMTCodec.FPA1,SampleRate=48000,Channels=2,SamplesPerChannel=2,Timestamp=123457,Data=au.AddrOfPinnedObject(),DataLength=16,FrameMetadata=mt.AddrOfPinnedObject(),FrameMetadataLength=meta.Length};
   OMTMediaFrame message=new OMTMediaFrame{Type=OMTFrameType.Metadata,Data=mt.AddrOfPinnedObject(),DataLength=meta.Length,Timestamp=789};
   for(int preview=0;preview<2;preview++) {
    IntPtr native=test_create();Check(native!=IntPtr.Zero,"C++ sender starts");
    try {using(var receiver=new OMTReceive("omt://127.0.0.1:26490",OMTFrameType.Video|OMTFrameType.Audio|OMTFrameType.Metadata,OMTPreferredVideoFormat.BGRA,(OMTReceiveFlags)(2|preview))) {
      OMTMediaFrame received=new OMTMediaFrame();
      Check(Until(()=>{omt_send(native,ref video);return receiver.Receive(OMTFrameType.Video,40,ref received);}),"C++ video -> official receiver");
      Check(received.Width==(preview==0?320:40)&&received.Height==(preview==0?176:22)&&received.Timestamp==video.Timestamp,"official receives full/preview dimensions and timestamp");
      Check(Until(()=>{omt_send(native,ref sound);return receiver.Receive(OMTFrameType.Audio,40,ref received);}),"C++ FPA1 -> official receiver");
      Check(Equal(Bytes(received.Data,received.DataLength),Bytes(sound.Data,16)) && Equal(Bytes(received.FrameMetadata,received.FrameMetadataLength),meta),"official audio samples and attached metadata exact");
      receiver.SetTally(new OMTTally(1,1));OMTTally tally=new OMTTally();
      Check(Until(()=>{omt_send_gettally(native,0,ref tally);return tally.Preview==1&&tally.Program==1;}),"official receiver tally -> C++ sender");
      Check(Until(()=>{receiver.GetTally(20,ref tally);return tally.Preview==1&&tally.Program==1;}),"C++ aggregate tally -> official receiver");
      Check(receiver.Send(message)>0,"official application metadata send");
      IntPtr result=omt_send_receive(native,2000);Check(result!=IntPtr.Zero,"C++ receives official application metadata");
      received=OMTMediaFrame.FromIntPtr(result);Check(received.Timestamp==789&&Equal(Bytes(received.Data,received.DataLength),meta),"metadata timestamp and C API NUL ownership");
    }}finally{omt_send_destroy(native);}
    using(var official=new OMTSend("InteropFixture",OMTQuality.Medium)) {
      // Pinned libomtnet overwrites audio samples when appending attached metadata.
      // Exercise its unmodified audio path without that separate upstream defect.
      OMTMediaFrame officialSound=sound;officialSound.FrameMetadata=IntPtr.Zero;officialSound.FrameMetadataLength=0;
      native=omt_receive_create("omt://127.0.0.1:"+official.Port,(OMTFrameType)7,OMTPreferredVideoFormat.BGRA,(OMTReceiveFlags)(2|preview));Check(native!=IntPtr.Zero,"C++ receiver starts");
      try {
       IntPtr result=IntPtr.Zero;
       Check(Until(()=>{official.Send(video);result=omt_receive(native,OMTFrameType.Video,40);return result!=IntPtr.Zero;}),"official video -> C++ receiver");
       var received=OMTMediaFrame.FromIntPtr(result);Check(received.Width==(preview==0?320:40)&&received.Height==(preview==0?176:22)&&received.Timestamp==123456,"C++ receives official full/preview geometry");
       Check(Until(()=>{official.Send(officialSound);result=omt_receive(native,OMTFrameType.Audio,40);return result!=IntPtr.Zero;}),"official FPA1 -> C++ receiver");
       received=OMTMediaFrame.FromIntPtr(result);Check(Equal(Bytes(received.Data,received.DataLength),Bytes(sound.Data,16)) && received.FrameMetadataLength==0,"C++ official audio samples exact");
       OMTTally tally=new OMTTally(1,1);omt_receive_settally(native,ref tally);
       Check(Until(()=>{official.GetTally(20,ref tally);return tally.Preview==1&&tally.Program==1;}),"C++ tally -> official sender");
       Check(Until(()=>{omt_receive_gettally(native,20,ref tally);return tally.Preview==1&&tally.Program==1;}),"official aggregate tally -> C++ receiver");
       Check(omt_receive_send(native,ref message)>0,"C++ metadata sent");
       Check(official.Receive(2000,ref received),"official receives C++ metadata");
       Check(received.Timestamp==789&&Equal(Bytes(received.Data,received.DataLength),meta),"official application metadata exact");
      }finally{omt_receive_destroy(native);}
    }
   }
  }finally{px.Free();au.Free();mt.Free();OMTDiscovery.GetInstance().Dispose();}
 }
}

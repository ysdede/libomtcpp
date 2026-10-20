// Exercise the actual bounded socket writer with real TCP and byte oracles.
// Include its translation unit to reach the internal helper; the static archive
// supplies AsyncPool only. No codec, synthetic encode success or timing sleeps.
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include "../src/OMTChannel.cpp"

static int failures = 0;
static void check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}
struct Pair {
    int tx = -1, rx = -1;
    explicit Pair(int receiveBuffer = 0) {
        int listener = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if (::bind(listener,reinterpret_cast<sockaddr*>(&addr),sizeof(addr)) || ::listen(listener,1)) std::exit(2);
        socklen_t size=sizeof(addr);
        if (::getsockname(listener,reinterpret_cast<sockaddr*>(&addr),&size)) std::exit(2);
        rx=static_cast<int>(::socket(AF_INET,SOCK_STREAM,0));
        if(receiveBuffer>0)setsockopt(rx,SOL_SOCKET,SO_RCVBUF,&receiveBuffer,sizeof(receiveBuffer));
        if (::connect(rx,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))) std::exit(2);
        tx=static_cast<int>(::accept(listener,nullptr,nullptr)); close(listener);
        if(tx<0 || fcntl(tx,F_SETFL,fcntl(tx,F_GETFL,0)|O_NONBLOCK)) std::exit(2);
        int one=1; setsockopt(tx,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
#ifdef SO_NOSIGPIPE
        setsockopt(tx,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
#ifdef _WIN32
        DWORD timeout=10000; setsockopt(rx,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
#else
        timeval timeout{10,0}; setsockopt(rx,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
#endif
    }
    ~Pair(){if(tx>=0)close(tx);if(rx>=0)close(rx);}
};
static bool readPattern(int fd,size_t length) {
    std::vector<uint8_t> buf(65536);size_t total=0;
    while(total<length){
        int n=static_cast<int>(recv(fd,buf.data(),static_cast<int>((std::min)(buf.size(),length-total)),0));
        if(n<=0)return false;
        for(int i=0;i<n;++i)if(buf[i]!=static_cast<uint8_t>((total+i)%251))return false;
        total+=n;
    }
    return true;
}
int main(int argc,char** argv){
    std::atomic<bool> alive{true},gone{false};
    const bool bench=argc>1;
    {
        Pair p;const int count=bench?20000:512;const size_t size=bench?1024:4096;
        std::vector<uint8_t> frame(size);bool received=false;bool sent=true;
        std::thread reader([&]{received=readPattern(p.rx,size*count);});
        const auto start=std::chrono::steady_clock::now();
        for(int i=0;i<count;++i){
            for(size_t j=0;j<size;++j)frame[j]=static_cast<uint8_t>((size*i+j)%251);
            size_t offset=0;
            if(omt::sendAllPoll(p.tx,frame.data(),frame.size(),alive,gone,offset,5000)!=omt::SendOutcome::Ok || offset!=frame.size()){sent=false;break;}
        }
        reader.join();
        const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        check(sent&&received,"fast writes preserve every byte in order");
        std::printf("MEASURE writes=%d bytes=%zu seconds=%.6f\n",count,size*count,elapsed);
    }
    if(bench)return failures?1:0;
    {
        Pair p(4096);int small=4096;setsockopt(p.tx,SOL_SOCKET,SO_SNDBUF,&small,sizeof(small));
        std::vector<uint8_t> payload(1024*1024);for(size_t i=0;i<payload.size();++i)payload[i]=static_cast<uint8_t>(i%251);
        size_t offset=0;
        check(omt::sendAllPoll(p.tx,payload.data(),4096,alive,gone,offset,1000)==omt::SendOutcome::Ok&&offset==4096,"prefix is committed before backpressure");
        auto start=std::chrono::steady_clock::now();
        auto outcome=omt::SendOutcome::Ok;
        while(offset<payload.size()&&outcome==omt::SendOutcome::Ok){
            const size_t next=(std::min)(payload.size(),offset+4096);
            outcome=omt::sendAllPoll(p.tx,payload.data(),next,alive,gone,offset,30);
        }
        std::printf("BACKPRESSURE offset=%zu total=%zu outcome=%d\n",offset,payload.size(),static_cast<int>(outcome));
        auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
        check(outcome==omt::SendOutcome::Timeout&&offset>0&&offset<payload.size(),"slow peer produces resumable partial write");
        check(ms<500,"slow-peer wait remains bounded");
        int roomy=1024*1024;setsockopt(p.rx,SOL_SOCKET,SO_RCVBUF,&roomy,sizeof(roomy));
        bool received=false;std::thread reader([&]{received=readPattern(p.rx,payload.size());});
        outcome=omt::sendAllPoll(p.tx,payload.data(),payload.size(),alive,gone,offset,8000);
        reader.join();check(outcome==omt::SendOutcome::Ok&&offset==payload.size()&&received,"resumed write has no duplicated or missing bytes");
    }
    {
        Pair p;uint8_t data[32]{};size_t offset=0;
        check(omt::sendAllPoll(p.tx,data,sizeof(data),alive,gone,offset,0)==omt::SendOutcome::Timeout&&offset==0,"expired budget sends no bytes");
        offset=0;alive=false;check(omt::sendAllPoll(p.tx,data,sizeof(data),alive,gone,offset,50)==omt::SendOutcome::Timeout&&offset==0,"cancellation sends no bytes");alive=true;
        offset=0;gone=true;check(omt::sendAllPoll(p.tx,data,sizeof(data),alive,gone,offset,50)==omt::SendOutcome::Timeout&&offset==0,"reader verdict prevents writes");gone=false;
        linger reset{1,0};setsockopt(p.rx,SOL_SOCKET,SO_LINGER,&reset,sizeof(reset));close(p.rx);p.rx=-1;
        std::vector<uint8_t> payload(1024*1024);offset=0;
        check(omt::sendAllPoll(p.tx,payload.data(),payload.size(),alive,gone,offset,1000)==omt::SendOutcome::PeerGone,"reset peer reports kernel closure");
    }
    return failures?1:0;
}

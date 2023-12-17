#include <filesystem>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <list>
#include <numeric>
#include <sgct/opengl.h>
#include <sgct/sgct.h>
#include <sgct/utils/dome.h>
#include <thread>
#include <ndi.h>
#include <mdns.h>
// #include <libavcodec/avcodec.h>
// #include <libavformat/avformat.h>
extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#include <errno.h>
#include <signal.h>
#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#define sleep(x) Sleep(x * 1000)
#else
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/time.h>
#endif

SwsContext* _videoScaleContext = nullptr;
AVFrame* _frame = nullptr; // holds src format frame
AVFrame* _tempFrame = nullptr; // holds dst format frame
AVPixelFormat _dstPixFmt = AV_PIX_FMT_BGR24;
int _videoWidth = 0;
int _videoHeight = 0;
int videoSizeChanged = 0;
int _tempFrameChanged = false;

using namespace sgct;

uint64_t frameNumber = 0;
float phi = glm::pi<float>();
float theta = 0.f;
int renderView = 2;
bool showId = false;
bool showStats = false;

ndi_frame_t frame;
ndi_video_format_t format;
ndi_codec_context_t codec_ctx;
std::list<ndi_packet_video_t *> _queue;
std::mutex _mutex;

// bool videochanged;
bool plane1changed;
bool plane2changed;
bool plane3changed;
bool size1changed;
bool size2changed;
bool size3changed;

// GLuint yuvtexture1 = 0;
// GLuint yuvtexture2 = 0;
// GLuint yuvtexture3 = 0;
// GLuint yuvtexture1_2 = 0;
// GLuint yuvtexture2_2 = 0;
// GLuint yuvtexture3_2 = 0;

GLuint textures[10];

GLuint visibleYTexture = 0;
GLuint visibleUTexture = 0;
GLuint visibleVTexture = 0;

int debugcounter1;
int debugcounter2;

// sgct::Image yuvvideoframe1;
// sgct::Image yuvvideoframe2;
// sgct::Image yuvvideoframe3;
std::unique_ptr<sgct::utils::Dome> dome;
GLint domeMvpMatrixLoc = -1;
GLint domeCameraMatrixLoc = -1;

std::string connectHost;
std::string connectPortStr;
int connectPort;

std::string detectedHost;
int detectedPort;

GLuint PBO;

static char addrbuffer[64];
static char entrybuffer[256];
static char namebuffer[256];
static char sendbuffer[1024];
static mdns_record_txt_t txtbuffer[128];

static struct sockaddr_in service_address_ipv4;
static struct sockaddr_in6 service_address_ipv6;

static int has_ipv4;
static int has_ipv6;

volatile sig_atomic_t running = 1;


constexpr std::string_view DomeVertexShader = R"(
  #version 330 core

  layout(location = 0) in vec2 texCoords;
  layout(location = 1) in vec3 normals;
  layout(location = 2) in vec3 vertPositions;

  uniform mat4 mvp;
  uniform mat4 camera;
  out vec2 uv;

  void main() {
    gl_Position =  mvp * camera * vec4(vertPositions, 1.0);
    uv = vec2(texCoords.x, 1.0 - texCoords.y);
  })";

constexpr std::string_view DomeFragmentShaderYUV = R"(
  #version 330 core

  uniform sampler2D tex1;
  uniform sampler2D tex2;
  uniform sampler2D tex3;

  vec3 offset = vec3(-0.0625, -0.5, -0.5);
  vec3 rcoeff = vec3(1.164, 0.000, 1.596);
  vec3 gcoeff = vec3(1.164, -0.391, -0.813);
  vec3 bcoeff = vec3(1.164, 2.018, 0.000);

  in vec2 uv;
  out vec4 color;

  void main() {
    vec4 t1 = texture(tex1, uv);
    vec4 t2 = texture(tex2, uv);
    vec4 t3 = texture(tex3, uv);
    vec3 yuv = vec3(t1.x, t2.x, t3.x) + offset;
    float r = dot(yuv, rcoeff);
    float g = dot(yuv, gcoeff);
    float b = dot(yuv, bcoeff);

    color = vec4(r, g, b, 1.0);
    // + vec4(uv.x / 4.0, uv.y / 4.0, 0, 0);
  }
)";

constexpr std::string_view DomeFragmentShaderRGB = R"(
  #version 330 core

  uniform sampler2D tex1;

  in vec2 uv;
  out vec4 color;

  void main() {
    vec4 t1 = texture(tex1, uv);
    color = t1
      + vec4(uv.x / 4.0, uv.y / 4.0, 0, 0);
  }
)";



static mdns_string_t
ipv4_address_to_string(char* buffer, size_t capacity, const struct sockaddr_in* addr,
    size_t addrlen) {
    char host[NI_MAXHOST] = { 0 };
    char service[NI_MAXSERV] = { 0 };
    int ret = getnameinfo((const struct sockaddr*)addr, (socklen_t)addrlen, host, NI_MAXHOST,
        service, NI_MAXSERV, NI_NUMERICSERV | NI_NUMERICHOST);
    int len = 0;
    if (ret == 0) {
        if (addr->sin_port != 0)
            len = snprintf(buffer, capacity, "%s:%s", host, service);
        else
            len = snprintf(buffer, capacity, "%s", host);
    }
    if (len >= (int)capacity)
        len = (int)capacity - 1;
    mdns_string_t str;
    str.str = buffer;
    str.length = len;
    return str;
}

static mdns_string_t
ipv6_address_to_string(char* buffer, size_t capacity, const struct sockaddr_in6* addr,
    size_t addrlen) {
    char host[NI_MAXHOST] = { 0 };
    char service[NI_MAXSERV] = { 0 };
    int ret = getnameinfo((const struct sockaddr*)addr, (socklen_t)addrlen, host, NI_MAXHOST,
        service, NI_MAXSERV, NI_NUMERICSERV | NI_NUMERICHOST);
    int len = 0;
    if (ret == 0) {
        if (addr->sin6_port != 0)
            len = snprintf(buffer, capacity, "[%s]:%s", host, service);
        else
            len = snprintf(buffer, capacity, "%s", host);
    }
    if (len >= (int)capacity)
        len = (int)capacity - 1;
    mdns_string_t str;
    str.str = buffer;
    str.length = len;
    return str;
}

static mdns_string_t
ip_address_to_string(char* buffer, size_t capacity, const struct sockaddr* addr, size_t addrlen) {
    if (addr->sa_family == AF_INET6)
        return ipv6_address_to_string(buffer, capacity, (const struct sockaddr_in6*)addr, addrlen);
    return ipv4_address_to_string(buffer, capacity, (const struct sockaddr_in*)addr, addrlen);
}

 
// Open sockets for sending one-shot multicast queries from an ephemeral port
static int
open_client_sockets(int* sockets, int max_sockets, int port) {
    // When sending, each socket can only send to one network interface
    // Thus we need to open one socket for each interface and address family
    int num_sockets = 0;

#ifdef _WIN32

    IP_ADAPTER_ADDRESSES* adapter_address = 0;
    ULONG address_size = 8000;
    unsigned int ret;
    unsigned int num_retries = 4;
    do {
        adapter_address = (IP_ADAPTER_ADDRESSES*)malloc(address_size);
        ret = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST, 0,
            adapter_address, &address_size);
        if (ret == ERROR_BUFFER_OVERFLOW) {
            free(adapter_address);
            adapter_address = 0;
            address_size *= 2;
        }
        else {
            break;
        }
    } while (num_retries-- > 0);

    if (!adapter_address || (ret != NO_ERROR)) {
        free(adapter_address);
        printf("Failed to get network adapter addresses\n");
        return num_sockets;
    }

    int first_ipv4 = 1;
    int first_ipv6 = 1;
    for (PIP_ADAPTER_ADDRESSES adapter = adapter_address; adapter; adapter = adapter->Next) {
        if (adapter->TunnelType == TUNNEL_TYPE_TEREDO)
            continue;
        if (adapter->OperStatus != IfOperStatusUp)
            continue;

        for (IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress; unicast;
            unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr->sa_family == AF_INET) {
                struct sockaddr_in* saddr = (struct sockaddr_in*)unicast->Address.lpSockaddr;
                if ((saddr->sin_addr.S_un.S_un_b.s_b1 != 127) ||
                    (saddr->sin_addr.S_un.S_un_b.s_b2 != 0) ||
                    (saddr->sin_addr.S_un.S_un_b.s_b3 != 0) ||
                    (saddr->sin_addr.S_un.S_un_b.s_b4 != 1)) {
                    int log_addr = 0;
                    if (first_ipv4) {
                        service_address_ipv4 = *saddr;
                        first_ipv4 = 0;
                        log_addr = 1;
                    }
                    has_ipv4 = 1;
                    if (num_sockets < max_sockets) {
                        saddr->sin_port = htons((unsigned short)port);
                        int sock = mdns_socket_open_ipv4(saddr);
                        if (sock >= 0) {
                            sockets[num_sockets++] = sock;
                            log_addr = 1;
                        }
                        else {
                            log_addr = 0;
                        }
                    }
                    if (log_addr) {
                        char buffer[128];
                        mdns_string_t addr = ipv4_address_to_string(buffer, sizeof(buffer), saddr,
                            sizeof(struct sockaddr_in));
                        printf("Local IPv4 address: %.*s\n", MDNS_STRING_FORMAT(addr));
                    }
                }
            }
            else if (unicast->Address.lpSockaddr->sa_family == AF_INET6) {
                struct sockaddr_in6* saddr = (struct sockaddr_in6*)unicast->Address.lpSockaddr;
                // Ignore link-local addresses
                if (saddr->sin6_scope_id)
                    continue;
                static const unsigned char localhost[] = { 0, 0, 0, 0, 0, 0, 0, 0,
                                                          0, 0, 0, 0, 0, 0, 0, 1 };
                static const unsigned char localhost_mapped[] = { 0, 0, 0,    0,    0,    0, 0, 0,
                                                                 0, 0, 0xff, 0xff, 0x7f, 0, 0, 1 };
                if ((unicast->DadState == NldsPreferred) &&
                    memcmp(saddr->sin6_addr.s6_addr, localhost, 16) &&
                    memcmp(saddr->sin6_addr.s6_addr, localhost_mapped, 16)) {
                    int log_addr = 0;
                    if (first_ipv6) {
                        service_address_ipv6 = *saddr;
                        first_ipv6 = 0;
                        log_addr = 1;
                    }
                    has_ipv6 = 1;
                    if (num_sockets < max_sockets) {
                        saddr->sin6_port = htons((unsigned short)port);
                        int sock = mdns_socket_open_ipv6(saddr);
                        if (sock >= 0) {
                            sockets[num_sockets++] = sock;
                            log_addr = 1;
                        }
                        else {
                            log_addr = 0;
                        }
                    }
                    if (log_addr) {
                        char buffer[128];
                        mdns_string_t addr = ipv6_address_to_string(buffer, sizeof(buffer), saddr,
                            sizeof(struct sockaddr_in6));
                        printf("Local IPv6 address: %.*s\n", MDNS_STRING_FORMAT(addr));
                    }
                }
            }
        }
    }

    free(adapter_address);

#else

    struct ifaddrs* ifaddr = 0;
    struct ifaddrs* ifa = 0;

    if (getifaddrs(&ifaddr) < 0)
        printf("Unable to get interface addresses\n");

    int first_ipv4 = 1;
    int first_ipv6 = 1;
    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr)
            continue;
        if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_MULTICAST))
            continue;
        if ((ifa->ifa_flags & IFF_LOOPBACK) || (ifa->ifa_flags & IFF_POINTOPOINT))
            continue;

        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in* saddr = (struct sockaddr_in*)ifa->ifa_addr;
            if (saddr->sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
                int log_addr = 0;
                if (first_ipv4) {
                    service_address_ipv4 = *saddr;
                    first_ipv4 = 0;
                    log_addr = 1;
                }
                has_ipv4 = 1;
                if (num_sockets < max_sockets) {
                    saddr->sin_port = htons(port);
                    int sock = mdns_socket_open_ipv4(saddr);
                    if (sock >= 0) {
                        sockets[num_sockets++] = sock;
                        log_addr = 1;
                    }
                    else {
                        log_addr = 0;
                    }
                }
                if (log_addr) {
                    char buffer[128];
                    mdns_string_t addr = ipv4_address_to_string(buffer, sizeof(buffer), saddr,
                        sizeof(struct sockaddr_in));
                    printf("Local IPv4 address: %.*s\n", MDNS_STRING_FORMAT(addr));
                }
            }
        }
        else if (ifa->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6* saddr = (struct sockaddr_in6*)ifa->ifa_addr;
            // Ignore link-local addresses
            if (saddr->sin6_scope_id)
                continue;
            static const unsigned char localhost[] = { 0, 0, 0, 0, 0, 0, 0, 0,
                                                      0, 0, 0, 0, 0, 0, 0, 1 };
            static const unsigned char localhost_mapped[] = { 0, 0, 0,    0,    0,    0, 0, 0,
                                                             0, 0, 0xff, 0xff, 0x7f, 0, 0, 1 };
            if (memcmp(saddr->sin6_addr.s6_addr, localhost, 16) &&
                memcmp(saddr->sin6_addr.s6_addr, localhost_mapped, 16)) {
                int log_addr = 0;
                if (first_ipv6) {
                    service_address_ipv6 = *saddr;
                    first_ipv6 = 0;
                    log_addr = 1;
                }
                has_ipv6 = 1;
                if (num_sockets < max_sockets) {
                    saddr->sin6_port = htons(port);
                    int sock = mdns_socket_open_ipv6(saddr);
                    if (sock >= 0) {
                        sockets[num_sockets++] = sock;
                        log_addr = 1;
                    }
                    else {
                        log_addr = 0;
                    }
                }
                if (log_addr) {
                    char buffer[128];
                    mdns_string_t addr = ipv6_address_to_string(buffer, sizeof(buffer), saddr,
                        sizeof(struct sockaddr_in6));
                    printf("Local IPv6 address: %.*s\n", MDNS_STRING_FORMAT(addr));
                }
            }
        }
    }

    freeifaddrs(ifaddr);

#endif

    return num_sockets;
}

// Open sockets to listen to incoming mDNS queries on port 5353
static int
open_service_sockets(int* sockets, int max_sockets) {
    // When recieving, each socket can recieve data from all network interfaces
    // Thus we only need to open one socket for each address family
    int num_sockets = 0;

    // Call the client socket function to enumerate and get local addresses,
    // but not open the actual sockets
    open_client_sockets(0, 0, 0);

    if (num_sockets < max_sockets) {
        struct sockaddr_in sock_addr;
        memset(&sock_addr, 0, sizeof(struct sockaddr_in));
        sock_addr.sin_family = AF_INET;
#ifdef _WIN32
        sock_addr.sin_addr = in4addr_any;
#else
        sock_addr.sin_addr.s_addr = INADDR_ANY;
#endif
        sock_addr.sin_port = htons(MDNS_PORT);
#ifdef __APPLE__
        sock_addr.sin_len = sizeof(struct sockaddr_in);
#endif
        int sock = mdns_socket_open_ipv4(&sock_addr);
        if (sock >= 0)
            sockets[num_sockets++] = sock;
    }

    if (num_sockets < max_sockets) {
        struct sockaddr_in6 sock_addr;
        memset(&sock_addr, 0, sizeof(struct sockaddr_in6));
        sock_addr.sin6_family = AF_INET6;
        sock_addr.sin6_addr = in6addr_any;
        sock_addr.sin6_port = htons(MDNS_PORT);
#ifdef __APPLE__
        sock_addr.sin6_len = sizeof(struct sockaddr_in6);
#endif
        int sock = mdns_socket_open_ipv6(&sock_addr);
        if (sock >= 0)
            sockets[num_sockets++] = sock;
    }

    return num_sockets;
}


// Callback handling parsing answers to queries sent
static int
query_callback(int sock, const struct sockaddr* from, size_t addrlen, mdns_entry_type_t entry,
    uint16_t query_id, uint16_t rtype, uint16_t rclass, uint32_t ttl, const void* data,
    size_t size, size_t name_offset, size_t name_length, size_t record_offset,
    size_t record_length, void* user_data) {
    (void)sizeof(sock);
    (void)sizeof(query_id);
    (void)sizeof(name_length);
    (void)sizeof(user_data);
    mdns_string_t fromaddrstr = ip_address_to_string(addrbuffer, sizeof(addrbuffer), from, addrlen);
    const char* entrytype = (entry == MDNS_ENTRYTYPE_ANSWER) ?
        "answer" :
        ((entry == MDNS_ENTRYTYPE_AUTHORITY) ? "authority" : "additional");
    mdns_string_t entrystr =
        mdns_string_extract(data, size, &name_offset, entrybuffer, sizeof(entrybuffer));
    if (rtype == MDNS_RECORDTYPE_PTR) {
        mdns_string_t namestr = mdns_record_parse_ptr(data, size, record_offset, record_length,
            namebuffer, sizeof(namebuffer));
        printf("%.*s : %s %.*s PTR %.*s rclass 0x%x ttl %u length %d\n",
            MDNS_STRING_FORMAT(fromaddrstr), entrytype, MDNS_STRING_FORMAT(entrystr),
            MDNS_STRING_FORMAT(namestr), rclass, ttl, (int)record_length);
    }
    else if (rtype == MDNS_RECORDTYPE_SRV) {
        mdns_record_srv_t srv = mdns_record_parse_srv(data, size, record_offset, record_length,
            namebuffer, sizeof(namebuffer));
        printf("%.*s : %s %.*s SRV %.*s priority %d weight %d port %d\n",
            MDNS_STRING_FORMAT(fromaddrstr), entrytype, MDNS_STRING_FORMAT(entrystr),
            MDNS_STRING_FORMAT(srv.name), srv.priority, srv.weight, srv.port);

        detectedPort = srv.port;
        printf("Detected NDI Source port %d\n", detectedPort);
    }
    else if (rtype == MDNS_RECORDTYPE_A) {
        struct sockaddr_in addr;
        mdns_record_parse_a(data, size, record_offset, record_length, &addr);
        mdns_string_t addrstr =
            ipv4_address_to_string(namebuffer, sizeof(namebuffer), &addr, sizeof(addr));
        printf("%.*s : %s %.*s A %.*s\n", MDNS_STRING_FORMAT(fromaddrstr), entrytype,
            MDNS_STRING_FORMAT(entrystr), MDNS_STRING_FORMAT(addrstr));

        detectedHost = std::string(addrstr.str);
        printf("Detected NDI Source host IP: %s\n", detectedHost.c_str());
    } 
    else {
        printf("%.*s : %s %.*s type %u rclass 0x%x ttl %u length %d\n",
            MDNS_STRING_FORMAT(fromaddrstr), entrytype, MDNS_STRING_FORMAT(entrystr), rtype,
            rclass, ttl, (int)record_length);
    }
    

    return 0;
}


void ndi_connect_thread() {
    int sockets[32];
    int query_id[32];
    int num_sockets = open_client_sockets(sockets, sizeof(sockets) / sizeof(sockets[0]), 0);
    if (num_sockets <= 0) {
        printf("Failed to open any client sockets\n");
        return;
    }
    printf("Opened %d socket%s for mDNS query\n", num_sockets, num_sockets ? "s" : "");

    size_t capacity = 2048;
    void* buffer = malloc(capacity);
    void* user_data = 0;

    mdns_query_t query[1] = {
        {
           .type = MDNS_RECORDTYPE_PTR,
           .name = "_ndi._tcp.local",
           .length = 0,
        },
    };
    query[0].length = strlen(query[0].name);
    int count = 1;

    do {

        printf("Sending mDNS query");
        for (size_t iq = 0; iq < count; ++iq) {
            const char* record_name = "PTR";
            if (query[iq].type == MDNS_RECORDTYPE_SRV)
                record_name = "SRV";
            else if (query[iq].type == MDNS_RECORDTYPE_A)
                record_name = "A";
            else if (query[iq].type == MDNS_RECORDTYPE_AAAA)
                record_name = "AAAA";
            else
                query[iq].type = MDNS_RECORDTYPE_PTR;
            printf(" : %s %s", query[iq].name, record_name);
        }
        printf("\n");


        for (int isock = 0; isock < num_sockets; ++isock) {
            query_id[isock] =
                mdns_multiquery_send(sockets[isock], query, count, buffer, capacity, 0);
            if (query_id[isock] < 0)
                printf("Failed to send mDNS query: %s\n", strerror(errno));
        }

        // This is a simple implementation that loops for 5 seconds or as long as we get replies
        int res;
        printf("Reading mDNS query replies\n");
        int records = 0;
        do {
            struct timeval timeout;
            timeout.tv_sec = 5;
            timeout.tv_usec = 0;

            int nfds = 0;
            fd_set readfs;
            FD_ZERO(&readfs);
            for (int isock = 0; isock < num_sockets; ++isock) {
                if (sockets[isock] >= nfds)
                    nfds = sockets[isock] + 1;
                FD_SET(sockets[isock], &readfs);
            }

            res = select(nfds, &readfs, 0, 0, &timeout);
            if (res > 0) {
                for (int isock = 0; isock < num_sockets; ++isock) {
                    if (FD_ISSET(sockets[isock], &readfs)) {
                        size_t rec = mdns_query_recv(sockets[isock], buffer, capacity, query_callback,
                            user_data, query_id[isock]);
                        if (rec > 0)
                            records += rec;
                    }
                    FD_SET(sockets[isock], &readfs);
                }
            }
        } while (res > 0);

        printf("Read %d records\n", records);

    } while (true);

    free(buffer);

    for (int isock = 0; isock < num_sockets; ++isock)
        mdns_socket_close(sockets[isock]);

    printf("Closed socket%s\n", num_sockets ? "s" : "");
     
}



void ndi_receiver_thread() {
    while (true) {
        if (!detectedHost.empty() && detectedPort > 0) {
            // NDI receive
            ndi_recv_context_t recv_ctx = ndi_recv_create();
            
            printf("Connecting to NDI source: %s:%d.\n", detectedHost.c_str(), detectedPort);

            int ret = ndi_recv_connect(recv_ctx, detectedHost.c_str(), detectedPort);
            if (ret < 0) {
              printf("Failed to connect to source\n");
            }
            printf("Connected.\n");

            ndi_packet_video_t video;
            ndi_packet_audio_t audio;
            ndi_packet_metadata_t meta;

            while (ndi_recv_is_connected(recv_ctx)) {

              int data_type = ndi_recv_capture(recv_ctx, &video, &audio, &meta, 50);
              switch (data_type) {

                  case NDI_DATA_TYPE_VIDEO:
                     // printf("Video data received (%dx%d %.4s).\n", video.width,video.height, (char*)&video.fourcc);
                    {
                      ndi_packet_video_t *clone =
                          (ndi_packet_video_t *)malloc(sizeof(ndi_packet_video_t));
                      memcpy(clone, &video, sizeof(ndi_packet_video_t));

                      _mutex.lock();
                      while (_queue.size() > 5) {
                        ndi_packet_video_t *v = _queue.back();
                        _queue.pop_back();
                        ndi_recv_free_video(v);
                        free(v);
                      }
                      _queue.push_back(clone);
                      _mutex.unlock();
                    }
                    break;

                  case NDI_DATA_TYPE_AUDIO:
                    // printf("Audio data received (%d samples).\n", audio.num_samples);
                    ndi_recv_free_audio(&audio);
                    break;

                  case NDI_DATA_TYPE_METADATA:
                    printf("Meta data received: %s\n", meta.data);
                    ndi_recv_free_metadata(&meta);
                    break;
                  }
            }

            printf("NDI disconnected.\n");
            ndi_recv_free(recv_ctx);


        }
        else {
            printf("No NDI source found yet.\n");
            sleep(1);
        }
    }
}


void ndi_decoder_thread() {
    while (true) {
        // Pop most recent frame off the queue
        ndi_packet_video_t* video = NULL;
        
        if (_queue.size() > 0) {
            _mutex.lock();
            video = _queue.front();
            _queue.pop_front();
            _mutex.unlock();
        }

        // Decode video if available
        if (video != NULL) {
            frame = ndi_codec_decode(codec_ctx, video);
            if (frame) {
                ndi_frame_get_format(frame, &format);

                // printf("ndi frame\n");

                // printf("got ndi frame: %d planes, %dx%d, chroma %dx%d\n", format.num_planes, format.width, format.height, format.chroma_width, format.chroma_height);
                 
                if (format.width != _videoWidth || format.height != _videoHeight) {
                    // printf("video size changed to %dx%d\n", format.width, format.height);
                    _videoWidth = format.width;
                    _videoHeight = format.height;
                    videoSizeChanged = true;
                }

                {
                    AVFrame* avframe = (AVFrame *)frame; // HACK

                    if (_videoScaleContext != nullptr && _tempFrame != nullptr) {
                        std::chrono::high_resolution_clock::time_point before, after;
                        std::chrono::duration<double, std::milli> duration;
                        before = std::chrono::high_resolution_clock::now();

                        const int scaleRet = sws_scale(
                            _videoScaleContext,
                            avframe->data,
                            avframe->linesize,
                            0,
                            _videoHeight,
                            _tempFrame->data,
                            _tempFrame->linesize
                        );


                        if (scaleRet < 0) {
                            sgct::Log::Error(
                                "Failed to convert decoded frame"
                            );
                        }
                        else {
                            after = std::chrono::high_resolution_clock::now();
                            duration = after - before;
                            if (debugcounter2++ % 100 == 0) {
                                printf("NDI frame %d x %d sws_scale time: %f ms\n", format.width, format.height, duration.count());
                            }

                            _tempFrameChanged = true;

                        }
                    }
                }

                /*
                {

                    std::chrono::high_resolution_clock::time_point before, after;
                    std::chrono::duration<double, std::milli> duration;
                    before = std::chrono::high_resolution_clock::now();

                    for (int i = 0; i < format.num_planes; i++) {
                        void* data = ndi_frame_get_data(frame, i);
                        int w = i ? format.chroma_width : format.width;
                        int h = i ? format.chroma_height : format.height;
                        int linesize = ndi_frame_get_linesize(frame, i);
                        w = linesize;

                        if (i == 0) {
                            if (w != yuvvideoframe1.size().x || h != yuvvideoframe1.size().y) {
                                printf("resizing yuvvideoframe1 to %dx%d\n", w, h);
                                yuvvideoframe1.setSize(ivec2(w, h));
                                yuvvideoframe1.allocateOrResizeData();
                                _videoWidth = w;
                                _videoHeight = h;
                                size1changed = true;
                            }
                            uint8_t* ptr = yuvvideoframe1.data();
                            memcpy(ptr, (unsigned char*)data, w * h);
                            plane1changed = true;
                        }

                        if (i == 1) {
                            if (w != yuvvideoframe2.size().x || h != yuvvideoframe2.size().y) {
                                printf("resizing yuvvideoframe2 to %dx%d\n", w, h);
                                yuvvideoframe2.setSize(ivec2(w, h));
                                yuvvideoframe2.allocateOrResizeData();
                                size2changed = true;
                            }
                            uint8_t* ptr = yuvvideoframe2.data();
                            memcpy(ptr, (unsigned char*)data, w * h);
                            plane2changed = true;
                        }

                        if (i == 2) {
                            if (w != yuvvideoframe3.size().x || h != yuvvideoframe3.size().y) {
                                printf("resizing yuvvideoframe3 to %dx%d\n", w, h);
                                yuvvideoframe3.setSize(ivec2(w, h));
                                yuvvideoframe3.allocateOrResizeData();
                                size3changed = true;
                            }
                            uint8_t* ptr = yuvvideoframe3.data();
                            memcpy(ptr, (unsigned char*)data, w * h);
                            plane3changed = true;
                        }

                    }

                    after = std::chrono::high_resolution_clock::now();
                    duration = after - before;
                    printf("NDI frame decode and copy time: %f ms\n", duration.count());
                }
                */

                ndi_frame_free(frame);
            }
            ndi_recv_free_video(video);
            free(video);
        }

    }
}


void initGL(GLFWwindow *) {
  ShaderManager::instance().addShaderProgram("xform", DomeVertexShader,
                                             DomeFragmentShaderRGB);
  const ShaderProgram &prog2 = ShaderManager::instance().shaderProgram("xform");
  prog2.bind();
  domeCameraMatrixLoc = glGetUniformLocation(prog2.id(), "camera");
  domeMvpMatrixLoc = glGetUniformLocation(prog2.id(), "mvp");
  glUniform1i(glGetUniformLocation(prog2.id(), "tex1"), 0);
  // glUniform1i(glGetUniformLocation(prog2.id(), "tex2"), 1);
  // glUniform1i(glGetUniformLocation(prog2.id(), "tex3"), 2);
  prog2.unbind();

  dome = std::make_unique<utils::Dome>(7.4f, 180.f, 256, 128);

  struct ImageData {
    std::string filename;
    Image img;
    std::atomic_bool imageDone = false;
    std::atomic_bool uploadDone = false;
    std::atomic_bool threadDone = false;
  };
  auto loadImage = [](ImageData &data) {
    data.img.load(data.filename);
    data.imageDone = true;
    while (!data.uploadDone) {
    }
    data.img = Image();
    data.threadDone = true;
  };

  Log::Info("Loading placeholder graphics...");

  // yuvvideoframe1.load("empty_y.png");
  // yuvvideoframe2.load("empty_u.png");
  // yuvvideoframe3.load("empty_v.png");

  size1changed = true;
  size2changed = true;
  size3changed = true;

  _videoWidth = 64;
  _videoHeight = 64;
  videoSizeChanged = true;

  GLuint tex;

  const int dataSize = 8192 * 8192 * 4;
  glGenBuffers(1, &PBO);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, PBO);
  glBufferData(GL_PIXEL_UNPACK_BUFFER, dataSize, 0, GL_DYNAMIC_DRAW);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  // TextureManager::instance().loadTexture(yuvvideoframe1);
  // TextureManager::instance().loadTexture(yuvvideoframe2);
  // TextureManager::instance().loadTexture(yuvvideoframe3);
  
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  glBindTexture(GL_TEXTURE_2D, 0);
}

void postSyncPreDraw() {
  Engine::instance().setStatsGraphVisibility(showStats);
}

static int debugrow = 0;

void preSync() {
    GLuint tex;

    if (videoSizeChanged) {

        // Recreate video context
        // printf("Video size changed: %d x %d\n", _videoWidth, _videoHeight);
        sgct::Log::Info(fmt::format("Resizing video buffers to {}x{}\n", _videoWidth, _videoHeight));

        _tempFrame = av_frame_alloc();
        if (!_tempFrame) {
            sgct::Log::Error("Could not allocate temp frame data");
            return;
        }

        _tempFrame->width = _videoWidth;
        _tempFrame->height = _videoHeight;
        _tempFrame->format = _dstPixFmt;

        const int ret = av_image_alloc(
            _tempFrame->data,
            _tempFrame->linesize,
            _videoWidth,
            _videoHeight,
            _dstPixFmt,
            1
        );
        if (ret < 0) {
            sgct::Log::Error(fmt::format("Could not allocate temp frame buffer ({} x {})", _videoWidth, _videoHeight));
            return;
        }

        if (!_frame) {
            _frame = av_frame_alloc();
        }

        if (!_frame) {
            sgct::Log::Error("Could not allocate frame data");
            return;
        }

        AVPixelFormat pix_fmt = AV_PIX_FMT_YUV422P;
        _videoScaleContext = sws_getContext(
            _videoWidth,
            _videoHeight,
            pix_fmt,
            _videoWidth,
            _videoHeight,
            _dstPixFmt,
            SWS_FAST_BILINEAR,
            nullptr,
            nullptr,
            nullptr
        );
        if (!_videoScaleContext) {
            sgct::Log::Error("Could not allocate frame conversion context");
            return;
        }

        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB8, _videoWidth, _videoHeight);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        textures[6] = tex;
        
        videoSizeChanged = false;
    }

    /*
    if (size1changed || size2changed || size3changed) {
        std::chrono::high_resolution_clock::time_point before, after;
        std::chrono::duration<double, std::milli> duration;
        before = std::chrono::high_resolution_clock::now();

        for (int k = 1; k < 8; k++) {
            // glDeleteTexure ...
        }
        
        // glBindBuffer(GL_PIXEL_UNPACK_BUFFER, PBO);

        if (size1changed) {
            uint32_t width = yuvvideoframe1.size().x;
            uint32_t height = yuvvideoframe1.size().y;

            for (int k = 0; k <= 1; k++) {
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, width, height);               
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture(GL_TEXTURE_2D, 0);
                textures[k] = tex;
            }
            size1changed = false;
        }

        if (size2changed) {
            uint32_t width = yuvvideoframe2.size().x;
            uint32_t height = yuvvideoframe2.size().y;

            for (int k = 2; k <= 3; k++) {
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, width, height);               
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture(GL_TEXTURE_2D, 0);
                textures[k] = tex;
            }
            size2changed = false;
        }

        if (size3changed) {
            uint32_t width = yuvvideoframe3.size().x;
            uint32_t height = yuvvideoframe3.size().y;

            for (int k = 4; k <= 5; k++) {
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, width ,height);            
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture(GL_TEXTURE_2D, 0);
                textures[k] = tex;
            }
            size3changed = false;
        }


        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);



        after = std::chrono::high_resolution_clock::now();
        duration = after - before;
        printf("GPU frame resize time: %f ms\n", duration.count());

    }
    */

  if (plane1changed || plane2changed || plane3changed || _tempFrameChanged) {

      std::chrono::high_resolution_clock::time_point before, after;
      std::chrono::duration<double, std::milli> duration;
      before = std::chrono::high_resolution_clock::now();

      // GLenum format = GL_UNSIGNED_BYTE;
      // GLenum type = GL_RED;          //  GL_BGRA;
      // GLenum internalFormat = GL_R8; // GL_RGBA8;

      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, PBO);

      if (_tempFrameChanged) {

          uint32_t width = _videoWidth;
          uint32_t height = _videoHeight;
          // uint8_t* data = yuvvideoframe1.data();
          uint8_t* data = (uint8_t*)_tempFrame->data[0];

          unsigned char* gpuMemory = reinterpret_cast<unsigned char*>(glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY));

          if (gpuMemory) {
              int dataOffset = 0;
              const int stride = width * 3;

              // printf("got memory 1 at %X, stride=%d\n", (long)gpuMemory, stride);

              for (int row = 0; row < height; row++) {
                  memcpy(gpuMemory + dataOffset, data + row * stride, stride);
                  dataOffset += stride;
              }

              /*
              for (int j = 0; j < 20; j++) {
                  for (int k = 0; k < width * 3; k++) {
                      gpuMemory[k + debugrow * width * 3] = rand() & 255;
                  } 

                  debugrow++;
                  if (debugrow > height) {
                      debugrow = 0;
                  }
              }
              */

              glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

              glActiveTexture(GL_TEXTURE0);

              glBindTexture(GL_TEXTURE_2D, textures[6]);

              glTexSubImage2D(
                  GL_TEXTURE_2D,
                  0,
                  0,
                  0,
                  _videoWidth,
                  _videoHeight,
                  GL_BGR,
                  GL_UNSIGNED_BYTE,
                  0
              );
              glBindTexture(GL_TEXTURE_2D, 0);
               
              _tempFrameChanged = false;

          }

      }

      /*
      if (plane1changed) {
          uint32_t width = yuvvideoframe1.size().x;
          uint32_t height = yuvvideoframe1.size().y;
          uint8_t* data = yuvvideoframe1.data();

          unsigned char* gpuMemory = reinterpret_cast<unsigned char*>(glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY));

          if (gpuMemory) {
              int dataOffset = 0;
              const int stride = width * 1;

              // printf("got memory 1 at %X\n", (long)gpuMemory);

              for (int row = 0; row < height; row++) {
                  memcpy(gpuMemory + dataOffset, data + row * stride, stride);
                  dataOffset += stride;
              }
               

              glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

              glActiveTexture(GL_TEXTURE0);

              glBindTexture(GL_TEXTURE_2D, textures[0]);
              if (visibleYTexture == 1) {
              }
              else {
              //    glBindTexture(GL_TEXTURE_2D, textures[1]);
              }
              // glBindTexture(GL_TEXTURE_2D, yuvtexture1);
             // glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, width, height);
              glTexSubImage2D(
                  GL_TEXTURE_2D,
                  0,
                  0,
                  0,
                  width,
                  height,
                  GL_RED,
                  GL_UNSIGNED_BYTE,
                  0
              );
              glBindTexture(GL_TEXTURE_2D, 0);

              if (visibleYTexture == 0) {
                  visibleYTexture = 1;
              }
              else {
                  visibleYTexture = 0;
              }

          }

          plane1changed = false;
      }
      */

      /*
      if (plane2changed) {
          uint32_t width = yuvvideoframe2.size().x;
          uint32_t height = yuvvideoframe2.size().y;
          uint8_t* data = yuvvideoframe2.data();

          unsigned char* gpuMemory = reinterpret_cast<unsigned char*>(glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY));

          if (gpuMemory) {
              int dataOffset = 0;
              const int stride = width * 1;

           
              for (int row = 0; row < height; row++) {
                  memcpy(gpuMemory + dataOffset, data + row * stride, stride);
                  dataOffset += stride;
              } 
               
              glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

              glActiveTexture(GL_TEXTURE0);
              glBindTexture(GL_TEXTURE_2D, textures[2]);
              if (visibleUTexture == 1) {
              }
              else {
               //   glBindTexture(GL_TEXTURE_2D, textures[3]);
              }
              // glBindTexture(GL_TEXTURE_2D, yuvtexture2);
              // glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, width, height);
              glTexSubImage2D(
                  GL_TEXTURE_2D,
                  0,
                  0,
                  0,
                  width,
                  height,
                  GL_RED,
                  GL_UNSIGNED_BYTE,
                  0
              );
              glBindTexture(GL_TEXTURE_2D, 0);

              if (visibleUTexture == 0) {
                  visibleUTexture = 1;
              }
              else {
                  visibleUTexture = 0;
              }
          }
          plane2changed = false;
      }

      if (plane3changed) {
          uint32_t width = yuvvideoframe3.size().x;
          uint32_t height = yuvvideoframe3.size().y;
          uint8_t* data = yuvvideoframe3.data();

          unsigned char* gpuMemory = reinterpret_cast<unsigned char*>(glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY));

          if (gpuMemory) {
              int dataOffset = 0;
              const int stride = width * 1;

             for (int row = 0; row < height; row++) {
                  memcpy(gpuMemory + dataOffset, data + row * stride, stride);
                  dataOffset += stride;
              }

              glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

              glActiveTexture(GL_TEXTURE0);

              glBindTexture(GL_TEXTURE_2D, textures[4]);
              if (visibleVTexture == 1) {
              }
              else {
               //    glBindTexture(GL_TEXTURE_2D, textures[5]);
              }
              // glBindTexture(GL_TEXTURE_2D, yuvtexture3);
              // glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, width, height);
              glTexSubImage2D(
                  GL_TEXTURE_2D,
                  0,
                  0,
                  0,
                  width,
                  height,
                  GL_RED,
                  GL_UNSIGNED_BYTE,
                  0
              );
              glBindTexture(GL_TEXTURE_2D, 0);

              if (visibleVTexture == 0) {
                  visibleVTexture = 1;
              }
              else {
                  visibleVTexture = 0;
              }
          }

          plane3changed = false;
      }
      */

       glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

       after = std::chrono::high_resolution_clock::now();
       duration = after - before;

       if (debugcounter1++ % 100 == 0) {
       printf("GPU upload time: %f ms\n", duration.count());
       }

  }
   
}

void draw(const RenderData& data) {
     
    // printf("render output frame\n");

    const mat4 mvp = data.modelViewProjectionMatrix;

    glm::vec3 direction = { std::cos(theta) * std::sin(phi), std::sin(theta),
                           std::cos(theta) * std::cos(phi) };
    glm::vec3 right = { std::sin(phi - glm::half_pi<float>()), 0.f,
                       std::cos(phi - glm::half_pi<float>()) };
    glm::vec3 up = glm::cross(right, direction);
    const glm::mat4 c = glm::lookAt(glm::vec3(0.f), direction, up);

    if (renderView == 2) {
        ShaderManager::instance().shaderProgram("xform").bind();

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures[6]);
        // glBindTexture(GL_TEXTURE_2D, textures[0]);
        if (visibleYTexture == 0) {
        }
        else {
            //   glBindTexture(GL_TEXTURE_2D, textures[1]);
        }

        // glActiveTexture(GL_TEXTURE1);
        // glBindTexture(GL_TEXTURE_2D, textures[2]);
         if (visibleUTexture == 0) {
         }
         else {
            //   glBindTexture(GL_TEXTURE_2D, textures[3]);
         }

        // glActiveTexture(GL_TEXTURE2);
        // glBindTexture(GL_TEXTURE_2D, textures[4]);
        if (visibleVTexture == 0) {
        }
        else {
            //    glBindTexture(GL_TEXTURE_2D, textures[5]);
        }

        glUniformMatrix4fv(domeMvpMatrixLoc, 1, GL_FALSE, mvp.values);
        glUniformMatrix4fv(domeCameraMatrixLoc, 1, GL_FALSE, glm::value_ptr(c));

        dome->draw();

        ShaderManager::instance().shaderProgram("xform").unbind();
    }
}

std::vector<std::byte> encode() {
  std::vector<std::byte> data;
  serializeObject(data, renderView);
  serializeObject(data, frameNumber);
  serializeObject(data, showId);
  serializeObject(data, showStats);
  serializeObject(data, theta);
  serializeObject(data, phi);
  return data;
}

void decode(const std::vector<std::byte> &data) {
  unsigned pos = 0;
  deserializeObject(data, pos, renderView);
  deserializeObject(data, pos, frameNumber);
  deserializeObject(data, pos, showId);
  deserializeObject(data, pos, showStats);
  deserializeObject(data, pos, theta);
  deserializeObject(data, pos, phi);
}

void cleanup() {
  // TextureManager::instance().removeTexture(yuvtexture1);
  // TextureManager::instance().removeTexture(yuvtexture2);
  // TextureManager::instance().removeTexture(yuvtexture3);
}

void keyboard(Key key, Modifier, Action action, int, Window *) {
  if (key == Key::Space && action == Action::Press) {
    renderView = (renderView + 1) % 3;
  }

  if (key == Key::Enter && action == Action::Press) {
    frameNumber = 0;
  }

  if (key == Key::R && action == Action::Press) {
    phi = glm::pi<float>();
    theta = 0.f;
  }

  if (key == Key::Esc && action == Action::Press) {
    Engine::instance().terminate();
  }

  if (key == Key::I && action == Action::Press) {
    showId = !showId;
  }

  if (key == Key::S && action == Action::Press) {
    showStats = !showStats;
  }

  if (key == Key::Left &&
      (action == Action::Press || action == Action::Repeat)) {
    phi += 0.1f;
    if (phi > glm::two_pi<float>()) {
      phi -= glm::two_pi<float>();
    }
  }

  if (key == Key::Right &&
      (action == Action::Press || action == Action::Repeat)) {
    phi -= 0.1f;
    if (phi < -glm::two_pi<float>()) {
      phi += glm::two_pi<float>();
    }
  }

  if (key == Key::Down &&
      (action == Action::Press || action == Action::Repeat)) {
    theta -= 0.1f;
    theta = std::clamp(theta, -glm::half_pi<float>(), glm::half_pi<float>());
  }

  if (key == Key::Up && (action == Action::Press || action == Action::Repeat)) {
    theta += 0.1f;
    theta = std::clamp(theta, -glm::half_pi<float>(), glm::half_pi<float>());
  }
}

int main(int argc, char **argv) {
  
  int ret;
  uint8_t *ptr;

  std::vector<std::string> arg(argv + 1, argv + argc);
  Configuration config = parseArguments(arg);
  config::Cluster cluster = loadCluster(config.configFilename);
  if (!cluster.success) {
    return -1;
  }

  for (int i = 0; i < argc; i++) {
      std::string_view argument = argv[i];
      if (argument == "--connect" && argc > i + 2) {
          connectHost = argv[i + 1];
          connectPortStr = argv[i + 2];
          connectPort = atoi(connectPortStr.c_str());
          Log::Info(fmt::format("Connecting to {}:{}", connectHost, connectPort));
      }
  }

#ifdef _WIN32

  WORD versionWanted = MAKEWORD(1, 1);
  WSADATA wsaData;
  if (WSAStartup(versionWanted, &wsaData)) {
      printf("Failed to initialize WinSock\n");
      return -1;
  }

#endif

  codec_ctx = ndi_codec_create();
  std::thread t1(ndi_connect_thread);
  std::thread t2(ndi_receiver_thread);
  std::thread t3(ndi_decoder_thread);

  Engine::Callbacks callbacks;
  
  callbacks.initOpenGL = initGL;
  callbacks.encode = encode;
  callbacks.decode = decode;
  callbacks.postSyncPreDraw = postSyncPreDraw;
  callbacks.preSync = preSync;
  callbacks.draw = draw;
  callbacks.cleanup = cleanup;
  callbacks.keyboard = keyboard;
  
  try {
    Engine::create(cluster, callbacks, config);
  } catch (const std::runtime_error &e) {
    Log::Error(e.what());
    Engine::destroy();
    return EXIT_FAILURE;
  }
  
  Log::Info("===========");
  Log::Info("Keybindings");
  Log::Info("LEFT:  Move camera pointing to the left");
  Log::Info("RIGHT: Move camera pointing to the right");
  Log::Info("UP:    Move camera pointing to up");
  Log::Info("DOWN:  Move camera pointing to down");
  Log::Info("ESC:   Terminate the program");
  Log::Info("I:     Show node id and IP");
  Log::Info("S:     Show statistics graphs");
  Log::Info("===========");
  
  Engine::instance().exec();
  Engine::destroy();

#ifdef _WIN32
  WSACleanup();
#endif

    exit(EXIT_SUCCESS);
}

#include "address.h"
#include "log.h"
#include <sstream>
#include <netdb.h>
#include <ifaddrs.h>
#include <stddef.h>
#include "endian.h"

namespace sylar {

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

//子网掩码取反
template <class T>
static T CreateMask(uint32_t bits) {
    //(1 << (32 - 24)) -1 = 00000000 00000000 00000000 11111111
    return (1 << (sizeof(T) * 8 - bits)) - 1;
}

//计算二进制中1的个数
template <class T>
static uint32_t CountBytes(T value) {
    uint32_t result = 0;
    //n-1：一个二进制的数减1，就是将这个二进制最右边的那个1变成0，然后它后边的所有位置
    //都变成1~ 举例：0011 0100，减1(n-1)后变成：0011 0011。
    //n &= (n-1)，并操作就会将有0的位置都变成0，上面的例子的结果就是0011 0000，然后
    //再赋值给n，这时n就去掉了一个1，或者叫做计数了一个1。以此类推，就可以得到1的个数。
    for (; value; ++result) {
        value &= value - 1;
    }
    
    return result;
}


Address::ptr Address::LookupAny(const std::string &host,
                            int family, int type, int protocol) {
    std::vector<Address::ptr> result;
    if (Lookup(result, host, family, type, protocol)) {
        return result[0];
    }
    return nullptr;
}

IPAddress::ptr Address::LookupAnyIPAddress(const std::string &host,
                                           int family, int type, int protocol) {
    std::vector<Address::ptr> result;
    if (Lookup(result, host, family, type, protocol)) {
        for (auto &i : result) {
            IPAddress::ptr v = std::dynamic_pointer_cast<IPAddress>(i);
            if (v) {
                return v;
            }
        }
    }
    return nullptr;
}

bool Address::Lookup(std::vector<Address::ptr> &result, const std::string &host,
                     int family, int type, int protocol) {
    addrinfo hints, *results, *next;
    hints.ai_flags     = 0;
    hints.ai_family    = family;
    hints.ai_socktype  = type;
    hints.ai_protocol  = protocol;
    hints.ai_addrlen   = 0;
    hints.ai_canonname = NULL;
    hints.ai_addr      = NULL;
    hints.ai_next      = NULL;

    std::string node;
    const char *service = NULL;

    //检查 ipv6address serivce, ipv6地址才解析
    if (!host.empty() && host[0] == '[') {
        const char *endipv6 = (const char *)memchr(host.c_str() + 1, ']', host.size() - 1);
        if (endipv6) {
            //TODO check out of range
            if (*(endipv6 + 1) == ':') {
                service = endipv6 + 2;
            }
            node = host.substr(1, endipv6 - host.c_str() - 1);
        }
    }

    //检查 node serivce 127.0.0.1:http
    if (node.empty()) {
        //void *memchr(const void *str, int c, size_t n) 在参数 str 所指向的字符串的前 n 个字节中搜索第一次出现字符 c（一个无符号字符）的位置。
        service = (const char *)memchr(host.c_str(), ':', host.size());
        if (service) {
            //service字符串里没有:
            if (!memchr(service + 1, ':', host.c_str() + host.size() - service - 1)) {
                node = host.substr(0, service - host.c_str());
                ++service;
            }
        }
    }

    if (node.empty()) {
        node = host;
    }
    int error = getaddrinfo(node.c_str(), service, &hints, &results);
    if (error) {
        SYLAR_LOG_DEBUG(g_logger) << "Address::Lookup getaddress(" << host << ", "
                                  << family << ", " << type << ") err=" << error << " errstr="
                                  << gai_strerror(error);
        return false;
    }

    next = results;
    while (next) {
        result.push_back(Create(next->ai_addr, (socklen_t)next->ai_addrlen));
        /// 一个ip/端口可以对应多种接字类型，比如SOCK_STREAM, SOCK_DGRAM, SOCK_RAW，所以这里会返回重复的结果
        SYLAR_LOG_DEBUG(g_logger) << "family:" << next->ai_family << ", sock type:" << next->ai_socktype;
        next = next->ai_next;
    }

    freeaddrinfo(results);
    return !result.empty();
}

bool Address::GetInterfaceAddresses(std::multimap<std::string, std::pair<Address::ptr, uint32_t>> &result,
                                    int family) {
    struct ifaddrs *next, *results;
    //getifaddrs创建一个链表，链表上的每个节点都是一个struct ifaddrs结构，getifaddrs
    //()返回链表第一个元素的指针。成功返回0, 失败返回-1,同时errno会被赋允相应错误码。
    //获取本地网络接口的信息。在路由器上可以用这个接口来获取wan/lan等接口当前的ip地址，广播地址等信息。
    if (getifaddrs(&results) != 0) {
        SYLAR_LOG_DEBUG(g_logger) << "Address::GetInterfaceAddresses getifaddrs "
                                     " err="
                                  << errno << " errstr=" << strerror(errno);
        return false;
    }

    try {
        for (next = results; next; next = next->ifa_next) {
            Address::ptr addr;
            uint32_t prefix_len = ~0u;
            //AF_UNSPEC适用于指定主机名和服务名且适合任何协议族的地址
            if (family != AF_UNSPEC && family != next->ifa_addr->sa_family) {
                continue;
            }
            switch (next->ifa_addr->sa_family) {
            case AF_INET: {
                addr             = Create(next->ifa_addr, sizeof(sockaddr_in));
                uint32_t netmask = ((sockaddr_in *)next->ifa_netmask)->sin_addr.s_addr;
                prefix_len       = CountBytes(netmask);
            } break;
            case AF_INET6: {
                addr              = Create(next->ifa_addr, sizeof(sockaddr_in6));
                in6_addr &netmask = ((sockaddr_in6 *)next->ifa_netmask)->sin6_addr;
                prefix_len        = 0;
                //ipv6地址一共128位，16*8个bit
                for (int i = 0; i < 16; ++i) {
                    prefix_len += CountBytes(netmask.s6_addr[i]);
                }
            } break;
            default:
                break;
            }

            if (addr) {
                result.insert(std::make_pair(next->ifa_name,
                                             std::make_pair(addr, prefix_len)));
            }
        }
    } catch (...) {
        SYLAR_LOG_ERROR(g_logger) << "Address::GetInterfaceAddresses exception";
        freeifaddrs(results);
        return false;
    }
    freeifaddrs(results);
    return !result.empty();
}

bool Address::GetInterfaceAddresses(std::vector<std::pair<Address::ptr, uint32_t>> &result, const std::string &iface, int family) {
    if (iface.empty() || iface == "*") {
        if (family == AF_INET || family == AF_UNSPEC) {
            result.push_back(std::make_pair(Address::ptr(new IPv4Address()), 0u));
        }
        if (family == AF_INET6 || family == AF_UNSPEC) {
            result.push_back(std::make_pair(Address::ptr(new IPv6Address()), 0u));
        }
        return true;
    }

    std::multimap<std::string, std::pair<Address::ptr, uint32_t>> results;

    if (!GetInterfaceAddresses(results, family)) {
        return false;
    }
    //查找值等于iface的子范围
    auto its = results.equal_range(iface);
    for (; its.first != its.second; ++its.first) {
        result.push_back(its.first->second);
    }
    return !result.empty();
}

int Address::getFamily() const {
    return getAddr()->sa_family;
}

std::string Address::toString() const {
    std::stringstream ss;
    insert(ss);
    return ss.str();
}

Address::ptr Address::Create(const sockaddr *addr, socklen_t addrlen) {
    if (addr == nullptr) {
        return nullptr;
    }

    Address::ptr result;
    switch (addr->sa_family) {
    case AF_INET:
        result.reset(new IPv4Address(*(const sockaddr_in *)addr));
        break;
    case AF_INET6:
        result.reset(new IPv6Address(*(const sockaddr_in6 *)addr));
        break;
    default:
        result.reset(new UnknownAddress(*addr));
        break;
    }
    return result;
}

bool Address::operator<(const Address &rhs) const {
    socklen_t minlen = std::min(getAddrLen(), rhs.getAddrLen());
    int result       = memcmp(getAddr(), rhs.getAddr(), minlen);
    if (result < 0) {//如果返回值 < 0，则表示 str1 小于 str2。
        return true;
    } else if (result > 0) {
        return false;
    } else if (getAddrLen() < rhs.getAddrLen()) {
        return true;
    }
    return false;
}

bool Address::operator==(const Address &rhs) const {
    return getAddrLen() == rhs.getAddrLen() && memcmp(getAddr(), rhs.getAddr(), getAddrLen()) == 0;
}

bool Address::operator!=(const Address &rhs) const {
    return !(*this == rhs);
}

IPAddress::ptr IPAddress::Create(const char *address, uint16_t port) {
    addrinfo hints, *results;
    memset(&hints, 0, sizeof(addrinfo));

    hints.ai_flags  = AI_NUMERICHOST;
    hints.ai_family = AF_UNSPEC;

    int error = getaddrinfo(address, NULL, &hints, &results);
    if (error) {
        SYLAR_LOG_DEBUG(g_logger) << "IPAddress::Create(" << address
                                  << ", " << port << ") error=" << error
                                  << " errno=" << errno << " errstr=" << strerror(errno);
        return nullptr;
    }
    //没看出来会抛异常
    try {
        IPAddress::ptr result = std::dynamic_pointer_cast<IPAddress>(
            Address::Create(results->ai_addr, (socklen_t)results->ai_addrlen));
        if (result) {
            result->setPort(port);
        }
        freeaddrinfo(results);
        return result;
    } catch (...) {
        freeaddrinfo(results);
        return nullptr;
    }
}

IPv4Address::ptr IPv4Address::Create(const char *address, uint16_t port) {
    IPv4Address::ptr rt(new IPv4Address);
    rt->m_addr.sin_port = sylar::byteswapOnLittleEndian(port);
    int result          = inet_pton(AF_INET, address, &rt->m_addr.sin_addr);
    if (result <= 0) {
        SYLAR_LOG_DEBUG(g_logger) << "IPv4Address::Create(" << address << ", "
                                  << port << ") rt=" << result << " errno=" << errno
                                  << " errstr=" << strerror(errno);
        return nullptr;
    }
    return rt;
}

IPv4Address::IPv4Address(const sockaddr_in &address) {
    m_addr = address;
}

IPv4Address::IPv4Address(uint32_t address, uint16_t port) {
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sin_family      = AF_INET;
    m_addr.sin_port        = sylar::byteswapOnLittleEndian(port);
    m_addr.sin_addr.s_addr = sylar::byteswapOnLittleEndian(address);
}

sockaddr *IPv4Address::getAddr() {
    return (sockaddr *)&m_addr;
}

const sockaddr *IPv4Address::getAddr() const {
    return (sockaddr *)&m_addr;
}

socklen_t IPv4Address::getAddrLen() const {
    return sizeof(m_addr);
}

std::ostream &IPv4Address::insert(std::ostream &os) const {
    uint32_t addr = sylar::byteswapOnLittleEndian(m_addr.sin_addr.s_addr);
    os << ((addr >> 24) & 0xff) << "."
       << ((addr >> 16) & 0xff) << "."
       << ((addr >> 8) & 0xff) << "."
       << (addr & 0xff);
    os << ":" << sylar::byteswapOnLittleEndian(m_addr.sin_port);
    return os;
}

IPAddress::ptr IPv4Address::broadcastAddress(uint32_t prefix_len) {
    if (prefix_len > 32) {
        return nullptr;
    }
    //子网掩码取反后与广播域或运算，即为广播地址
    sockaddr_in baddr(m_addr);
    baddr.sin_addr.s_addr |= sylar::byteswapOnLittleEndian(
        CreateMask<uint32_t>(prefix_len));
    return IPv4Address::ptr(new IPv4Address(baddr));
}

IPAddress::ptr IPv4Address::networkAddress(uint32_t prefix_len) {
    if (prefix_len > 32) {
        return nullptr;
    }
    //ip与子网掩码与运算，即为广播域
    sockaddr_in baddr(m_addr);
    baddr.sin_addr.s_addr &= sylar::byteswapOnLittleEndian(
        ~CreateMask<uint32_t>(prefix_len));
    return IPv4Address::ptr(new IPv4Address(baddr));
}

IPAddress::ptr IPv4Address::subnetMask(uint32_t prefix_len) {
    sockaddr_in subnet;
    memset(&subnet, 0, sizeof(subnet));
    subnet.sin_family      = AF_INET;
    //255.255.255.0
    subnet.sin_addr.s_addr = ~sylar::byteswapOnLittleEndian(CreateMask<uint32_t>(prefix_len));
    return IPv4Address::ptr(new IPv4Address(subnet));
}

uint32_t IPv4Address::getPort() const {
    return sylar::byteswapOnLittleEndian(m_addr.sin_port);
}

void IPv4Address::setPort(uint16_t v) {
    m_addr.sin_port = sylar::byteswapOnLittleEndian(v);
}

IPv6Address::ptr IPv6Address::Create(const char *address, uint16_t port) {
    IPv6Address::ptr rt(new IPv6Address);
    rt->m_addr.sin6_port = sylar::byteswapOnLittleEndian(port);
    int result           = inet_pton(AF_INET6, address, &rt->m_addr.sin6_addr);
    if (result <= 0) {
        SYLAR_LOG_DEBUG(g_logger) << "IPv6Address::Create(" << address << ", "
                                  << port << ") rt=" << result << " errno=" << errno
                                  << " errstr=" << strerror(errno);
        return nullptr;
    }
    return rt;
}

IPv6Address::IPv6Address() {
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sin6_family = AF_INET6;
}

IPv6Address::IPv6Address(const sockaddr_in6 &address) {
    m_addr = address;
}

IPv6Address::IPv6Address(const uint8_t address[16], uint16_t port) {
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sin6_family = AF_INET6;
    m_addr.sin6_port   = sylar::byteswapOnLittleEndian(port);
    memcpy(&m_addr.sin6_addr.s6_addr, address, 16);
}

sockaddr *IPv6Address::getAddr() {
    return (sockaddr *)&m_addr;
}

const sockaddr *IPv6Address::getAddr() const {
    return (sockaddr *)&m_addr;
}

socklen_t IPv6Address::getAddrLen() const {
    return sizeof(m_addr);
}

std::ostream &IPv6Address::insert(std::ostream &os) const {
    os << "[";
    uint16_t *addr  = (uint16_t *)m_addr.sin6_addr.s6_addr;
    bool used_zeros = false;
    for (size_t i = 0; i < 8; ++i) {
        //如果一个以冒号十六进制数表示法表示的IPv6地址中，如果几个连续的段值都是0，
        //那么这些0可以简记为::。每个地址中只能有一个::
        if (addr[i] == 0 && !used_zeros) {
            continue;
        }
        //上一位数字为0
        if (i && addr[i - 1] == 0 && !used_zeros) {
            os << ":";
            used_zeros = true;
        }
        if (i) {
            os << ":";
        }
        os << std::hex << (int)sylar::byteswapOnLittleEndian(addr[i]) << std::dec;
    }

    if (!used_zeros && addr[7] == 0) {
        os << "::";
    }

    os << "]:" << sylar::byteswapOnLittleEndian(m_addr.sin6_port);
    return os;
}

IPAddress::ptr IPv6Address::broadcastAddress(uint32_t prefix_len) {
    sockaddr_in6 baddr(m_addr);
     //子网掩码取反后与广播域或运算，即为广播地址
    baddr.sin6_addr.s6_addr[prefix_len / 8] |=
        CreateMask<uint8_t>(prefix_len % 8);
    for (int i = prefix_len / 8 + 1; i < 16; ++i) {
        baddr.sin6_addr.s6_addr[i] = 0xff;
    }
    return IPv6Address::ptr(new IPv6Address(baddr));
}

IPAddress::ptr IPv6Address::networkAddress(uint32_t prefix_len) {
    sockaddr_in6 baddr(m_addr);
    //ip与子网掩码与运算，即为广播域
    baddr.sin6_addr.s6_addr[prefix_len / 8] &=
        CreateMask<uint8_t>(prefix_len % 8);
    for (int i = prefix_len / 8 + 1; i < 16; ++i) {
        baddr.sin6_addr.s6_addr[i] = 0x00;
    }
    return IPv6Address::ptr(new IPv6Address(baddr));
}

IPAddress::ptr IPv6Address::subnetMask(uint32_t prefix_len) {
    sockaddr_in6 subnet;
    memset(&subnet, 0, sizeof(subnet));
    subnet.sin6_family = AF_INET6;
    subnet.sin6_addr.s6_addr[prefix_len / 8] =
        ~CreateMask<uint8_t>(prefix_len % 8);
    //每字节子网掩码是11111111
    for (uint32_t i = 0; i < prefix_len / 8; ++i) {
        subnet.sin6_addr.s6_addr[i] = 0xff;
    }
    return IPv6Address::ptr(new IPv6Address(subnet));
}

uint32_t IPv6Address::getPort() const {
    return sylar::byteswapOnLittleEndian(m_addr.sin6_port);
}

void IPv6Address::setPort(uint16_t v) {
    m_addr.sin6_port = sylar::byteswapOnLittleEndian(v);
}


static const size_t MAX_PATH_LEN = sizeof(((sockaddr_un *)0)->sun_path) - 1;//127

UnixAddress::UnixAddress() {
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sun_family = AF_UNIX;
    m_length          = offsetof(sockaddr_un, sun_path) + MAX_PATH_LEN;//不含\0
}

UnixAddress::UnixAddress(const std::string &path) {
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sun_family = AF_UNIX;
    m_length          = path.size() + 1;//这里含\0

    if (!path.empty() && path[0] == '\0') {//sun_path为空的情况
        --m_length;
    }

    if (m_length > sizeof(m_addr.sun_path)) {
        throw std::logic_error("path too long");
    }
    memcpy(m_addr.sun_path, path.c_str(), m_length);
    m_length += offsetof(sockaddr_un, sun_path);
}

void UnixAddress::setAddrLen(uint32_t v) {
    m_length = v;
}

sockaddr *UnixAddress::getAddr() {
    return (sockaddr *)&m_addr;
}

const sockaddr *UnixAddress::getAddr() const {
    return (sockaddr *)&m_addr;
}

socklen_t UnixAddress::getAddrLen() const {
    return m_length;
}

std::string UnixAddress::getPath() const {
    std::stringstream ss;
    if (m_length > offsetof(sockaddr_un, sun_path) && m_addr.sun_path[0] == '\0') {
        ss << "\\0" << std::string(m_addr.sun_path + 1, m_length - offsetof(sockaddr_un, sun_path) - 1);
    } else {
        ss << m_addr.sun_path;
    }
    return ss.str();
}

std::ostream &UnixAddress::insert(std::ostream &os) const {
    if (m_length > offsetof(sockaddr_un, sun_path) && m_addr.sun_path[0] == '\0') {
        return os << "\\0" << std::string(m_addr.sun_path + 1, m_length - offsetof(sockaddr_un, sun_path) - 1);
    }
    return os << m_addr.sun_path;
}

UnknownAddress::UnknownAddress(int family) {
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sa_family = family;
}

UnknownAddress::UnknownAddress(const sockaddr &addr) {
    m_addr = addr;
}

sockaddr *UnknownAddress::getAddr() {
    return (sockaddr *)&m_addr;
}

const sockaddr *UnknownAddress::getAddr() const {
    return &m_addr;
}

socklen_t UnknownAddress::getAddrLen() const {
    return sizeof(m_addr);
}

std::ostream &UnknownAddress::insert(std::ostream &os) const {
    os << "[UnknownAddress family=" << m_addr.sa_family << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const Address &addr) {
    return addr.insert(os);
}

}
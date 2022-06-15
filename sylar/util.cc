#include <unistd.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <signal.h> // for kill()
#include <sys/syscall.h>
#include <sys/stat.h>
#include <execinfo.h> // for backtrace()
#include <cxxabi.h>   // for abi::__cxa_demangle()
#include <algorithm>  // for std::transform()
#include "util.h"
#include "log.h"
#include "fiber.h"

namespace sylar {

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

pid_t GetThreadId() {
  return syscall(SYS_gettid);
}

uint64_t GetFiberId() {
    return Fiber::GetFiberId();
}

uint64_t GetElapsedMS() {
    struct timespec ts = {0};
    //从系统启动这一刻起开始计时
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

std::string GetThreadName() {
    char thread_name[16] = {0};
    pthread_getname_np(pthread_self(), thread_name, 16);
    return std::string(thread_name);
}

void SetThreadName(const std::string &name) {
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
}

//__cxa_demangle是C++的函数，而且是ABI（应用程序二进制接口），用于将已被编译器转换后的
//函数名给还原为原来的形式（编译的时候加上-rdunamic选项，加入函数符号表才能正确显示）
static std::string demangle(const char *str) {
    size_t size = 0;
    int status  = 0;
    std::string rt;
    rt.resize(256);
    if (1 == sscanf(str, "%*[^(]%*[^_]%255[^)+]", &rt[0])) {
        char *v = abi::__cxa_demangle(&rt[0], nullptr, &size, &status);
        if (v) {
            std::string result(v);
            free(v);
            return result;
        }
    }
    if (1 == sscanf(str, "%255s", &rt[0])) {
        return rt;
    }
    return str;
}

void Backtrace(std::vector<std::string> &bt, int size, int skip) {
    void **array = (void **)malloc((sizeof(void *) * size));
    //backtrace函数会将当前程序的调用堆栈信息写入buffer所指向的数组。buffer中的每一项
    //都是void *类型的，是对应堆栈帧的返回地址。而size参数指定可以存储在缓冲区中的最大
    //地址数，如果回溯大于size，则返回最近size个调用堆栈信息，为了获得完整的回溯，我们
    //必须确保缓冲区和大小足够大.返回值：返回获取到的调用堆栈信息的数量，该值不大于size。
    size_t s     = ::backtrace(array, size);
    //第一个参数是backtrace返回信息buffer，backtrace_symbols的功能将地址信息转换为
    //一个字符串数组，用于描述堆栈信息。size参数指定缓冲区中地址的数量
    char **strings = backtrace_symbols(array, s);
    if (strings == NULL) {
        SYLAR_LOG_ERROR(g_logger) << "backtrace_synbols error";
        return;
    }

    for (size_t i = skip; i < s; ++i) {
        bt.push_back(demangle(strings[i]));
    }

    free(strings);
    free(array);
}

std::string BacktraceToString(int size, int skip, const std::string &prefix) {
    std::vector<std::string> bt;
    Backtrace(bt, size, skip);
    std::stringstream ss;
    for (size_t i = 0; i < bt.size(); ++i) {
        ss << prefix << bt[i] << std::endl;
    }
    return ss.str();
}

uint64_t GetCurrentMS() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000ul + tv.tv_usec / 1000;
}

uint64_t GetCurrentUS() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 * 1000ul + tv.tv_usec;
}

std::string ToUpper(const std::string &name) {
    std::string rt = name;
    std::transform(rt.begin(), rt.end(), rt.begin(), ::toupper);
    return rt;
}

std::string ToLower(const std::string &name) {
    std::string rt = name;
    std::transform(rt.begin(), rt.end(), rt.begin(), ::tolower);
    return rt;
}

std::string Time2Str(time_t ts, const std::string &format) {
    struct tm tm;
    localtime_r(&ts, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), format.c_str(), &tm);
    return buf;
}

time_t Str2Time(const char *str, const char *format) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    if (!strptime(str, format, &t)) {
        return 0;
    }
    //time_t mktime(struct tm *timeptr) 把 timeptr 所指向的结构转换为自1970年1月1日以来持续时间的秒数
    return mktime(&t);
}

void FSUtil::ListAllFile(std::vector<std::string> &files, const std::string &path, const std::string &subfix) {
    //检查调用进程是否可以对指定的文件执行某种操作,F_OK 值为0，判断文件是否存在
    if (access(path.c_str(), 0) != 0) {
        return;
    }
    //打开参数指定的目录, 并返回DIR*形态的目录流, 和open()类似, 接下来对目录的读取和搜索都要使用此返回值
    DIR *dir = opendir(path.c_str());
    if (dir == nullptr) {
        return;
    }
    //dirent:文件或者目录结构体
    struct dirent *dp = nullptr;
    //readdir需要一个已打开（调用opendir）的DIR对象作为参数,readdir()返回参数dir目录流的下个目录进入点。
    while ((dp = readdir(dir)) != nullptr) {
        //d_type文件类型是目录
        if (dp->d_type == DT_DIR) {
            if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) {
                continue;
            }
            ListAllFile(files, path + "/" + dp->d_name, subfix);
        } else if (dp->d_type == DT_REG) { //常规文件
            //获取文件名
            std::string filename(dp->d_name);
            if (subfix.empty()) {
                files.push_back(path + "/" + filename);
            } else {
                //文件名比后缀还短
                if (filename.size() < subfix.size()) {
                    continue;
                }
                if (filename.substr(filename.length() - subfix.size()) == subfix) {
                    files.push_back(path + "/" + filename);
                }
            }
        }
    }
    closedir(dir);//关闭参数dir 所指的目录流
}

static int __lstat(const char *file, struct stat *st = nullptr) {
    struct stat lst;
    //当文件为符号连接时, lstat()会返回该link 本身的状态,成功执行时，返回0,失败返回-1
    //注意lst这里一定要进行初始化，说明其为一块有效的内存空间
    int ret = lstat(file, &lst);
    if (st) {
        *st = lst;
    }
    return ret;
}

static int __mkdir(const char *dirname) {
    //判断文件是否存在
    if (access(dirname, F_OK) == 0) {
        return 0;
    }
    //S_IRWXU 00700权限，代表该文件所有者拥有读，写和执行操作的权限
    //S_IRWXG 00070权限，代表该文件用户组拥有读，写和执行操作的权限
    //S_IROTH 00004权限，代表其他用户拥有可读的权限
    //S_IXOTH 00001权限，代表其他用户拥有执行的权限
    return mkdir(dirname, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
}

bool FSUtil::Mkdir(const std::string &dirname) {
    if (__lstat(dirname.c_str()) == 0) {
        return true;
    }
    //strdup()会先用maolloc()配置与参数s字符串相同的空间大小，然后将参数s字符串的内容
    //复制到该内存地址，然后把该地址返回。
    char *path = strdup(dirname.c_str());
    char *ptr  = strchr(path + 1, '/');
    do {
        //循环结束时ptr=nullptr
        for (; ptr; *ptr = '/', ptr = strchr(ptr + 1, '/')) {
            *ptr = '\0';
            //递归的创建文件目录，若目录创建成功，则返回0
            if (__mkdir(path) != 0) {
                break;
            }
        }
        //创建目录失败
        if (ptr != nullptr) {
            break;
        } else if (__mkdir(path) != 0) {
            break;
        }
        free(path);
        return true;
    } while (0);
    free(path);
    return false;
}

bool FSUtil::IsRunningPidfile(const std::string &pidfile) {
    if (__lstat(pidfile.c_str()) != 0) {
        return false;
    }
    std::ifstream ifs(pidfile);
    std::string line;
    if (!ifs || !std::getline(ifs, line)) {
        return false;
    }
    if (line.empty()) {
        return false;
    }
    //从文件中取出pid，转为pid_t2
    pid_t pid = atoi(line.c_str());
    if (pid <= 1) {
        return false;
    }
    //kill -0 pid 不发送任何信号，但是系统会进行错误检查。
    //所以经常用来检查一个进程是否存在，存在返回0；不存在返回1
    if (kill(pid, 0) != 0) {
        return false;
    }
    return true;
}

bool FSUtil::Unlink(const std::string &filename, bool exist) {
    //文件不存在，取不到信息
    if (!exist && __lstat(filename.c_str())) {
        return true;
    }
    return ::unlink(filename.c_str()) == 0;
}

bool FSUtil::Rm(const std::string &path) {
    struct stat st;
    //返回非0，表示不存在文件
    if (lstat(path.c_str(), &st)) {
        return true;
    }
    // 文件类型不是目录
    if (!(st.st_mode & S_IFDIR)) {
        return Unlink(path);
    }

    DIR *dir = opendir(path.c_str());
    if (!dir) {
        return false;
    }

    bool ret          = true;
    struct dirent *dp = nullptr;
    //递归删除目录下文件
    while ((dp = readdir(dir))) {
        if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) {
            continue;
        }
        std::string dirname = path + "/" + dp->d_name;
        ret                 = Rm(dirname);
    }
    closedir(dir);
    //删除空的目录
    if (::rmdir(path.c_str())) {
        ret = false;
    }
    return ret;
}

bool FSUtil::Mv(const std::string &from, const std::string &to) {
    //先删除目标文件
    if (!Rm(to)) {
        return false;
    }
    //如果newname与oldname不在一个目录下，则相当于移动文件。
    return rename(from.c_str(), to.c_str()) == 0;
}

bool FSUtil::Realpath(const std::string &path, std::string &rpath) {
    if (__lstat(path.c_str())) {
        return false;
    }
    char *ptr = ::realpath(path.c_str(), nullptr);
    if (nullptr == ptr) {
        return false;
    }
    std::string(ptr).swap(rpath);
    free(ptr);
    return true;
}

bool FSUtil::Symlink(const std::string &from, const std::string &to) {
    if (!Rm(to)) {
        return false;
    }
    //symlink()创建一个符号链接，其中包含from字符串。
    return ::symlink(from.c_str(), to.c_str()) == 0;
}

std::string FSUtil::Dirname(const std::string &filename) {
    if (filename.empty()) {
        return ".";
    }
    //反向查找第一个'/'
    auto pos = filename.rfind('/');
    if (pos == 0) {
        return "/";
    } else if (pos == std::string::npos) {
        return ".";
    } else {
        return filename.substr(0, pos);
    }
}

std::string FSUtil::Basename(const std::string &filename) {
    if (filename.empty()) {
        return filename;
    }
    auto pos = filename.rfind('/');
    if (pos == std::string::npos) {
        return filename;
    } else {
        return filename.substr(pos + 1);
    }
}

bool FSUtil::OpenForRead(std::ifstream &ifs, const std::string &filename, std::ios_base::openmode mode) {
    ifs.open(filename.c_str(), mode);//mode模式打开
    return ifs.is_open();
}

bool FSUtil::OpenForWrite(std::ofstream &ofs, const std::string &filename, std::ios_base::openmode mode) {
    ofs.open(filename.c_str(), mode);
    if (!ofs.is_open()) {
        //取目录部分
        std::string dir = Dirname(filename);
        Mkdir(dir);
        ofs.open(filename.c_str(), mode);
    }
    return ofs.is_open();
}

int8_t TypeUtil::ToChar(const std::string &str) {
    if (str.empty()) {
        return 0;
    }
    return *str.begin();
}

int64_t TypeUtil::Atoi(const std::string &str) {
    if (str.empty()) {
        return 0;
    }
    //str 为要转换的字符串，endstr 为第一个不能转换的字符的指针,为nullptr表示不关心，
    //base 为字符串 str 所采用的进制。
    return strtoull(str.c_str(), nullptr, 10);
}

double TypeUtil::Atof(const std::string &str) {
    if (str.empty()) {
        return 0;
    }
    return atof(str.c_str());
}

int8_t TypeUtil::ToChar(const char *str) {
    if (str == nullptr) {
        return 0;
    }
    return str[0];
}

int64_t TypeUtil::Atoi(const char *str) {
    if (str == nullptr) {
        return 0;
    }

    return strtoull(str, nullptr, 10);
}

double TypeUtil::Atof(const char *str) {
    if (str == nullptr) {
        return 0;
    }
    return atof(str);
}

std::string StringUtil::Format(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    auto v = Formatv(fmt, ap);
    va_end(ap);
    return v;
}

std::string StringUtil::Formatv(const char* fmt, va_list ap) {
    char* buf = nullptr;
    auto len = vasprintf(&buf, fmt, ap);
    if(len == -1) {
        return "";
    }
    std::string ret(buf, len);
    free(buf);
    return ret;
}


static const char uri_chars[256] = {
    /* 0 */
    0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 1, 1, 0,
    1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 0, 0, 0, 1, 0, 0,
    /* 64 */
    0, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 0, 0, 0, 0, 1,
    0, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 0, 0, 0, 1, 0,
    /* 128 */
    0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
    /* 192 */
    0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
};

static const char xdigit_chars[256] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,1,2,3,4,5,6,7,8,9,0,0,0,0,0,0,
    0,10,11,12,13,14,15,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,10,11,12,13,14,15,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

#define CHAR_IS_UNRESERVED(c)           \
    (uri_chars[(unsigned char)(c)])

//-.0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz~
//字符'a'-'z','A'-'Z','0'-'9','.','-','*'和'_'都不被编码，维持原值；
//空格' '被转换为加号'+'。
//其他每个字节都被表示成%XY的格式，X和Y分别代表一个十六进制位。编码为UTF-8。
std::string StringUtil::UrlEncode(const std::string& str, bool space_as_plus) {
    static const char *hexdigits = "0123456789ABCDEF";
    std::string* ss = nullptr;
    const char* end = str.c_str() + str.length();
    for(const char* c = str.c_str() ; c < end; ++c) {
        if(!CHAR_IS_UNRESERVED(*c)) {
            if(!ss) {
                ss = new std::string;
                ss->reserve(str.size() * 1.2);
                ss->append(str.c_str(), c - str.c_str());
            }
            if(*c == ' ' && space_as_plus) {
                ss->append(1, '+');
            } else {
                ss->append(1, '%');
                ss->append(1, hexdigits[(uint8_t)*c >> 4]);
                ss->append(1, hexdigits[*c & 0xf]);
            }
        } else if(ss) {
            ss->append(1, *c);
        }
    }
    if(!ss) {
        return str;
    } else {
        std::string rt = *ss;
        delete ss;
        return rt;
    }
}

std::string StringUtil::UrlDecode(const std::string& str, bool space_as_plus) {
    std::string* ss = nullptr;
    const char* end = str.c_str() + str.length();
    for(const char* c = str.c_str(); c < end; ++c) {
        if(*c == '+' && space_as_plus) {
            if(!ss) {
                ss = new std::string;
                ss->append(str.c_str(), c - str.c_str());
            }
            ss->append(1, ' ');
        } else if(*c == '%' && (c + 2) < end
                    && isxdigit(*(c + 1)) && isxdigit(*(c + 2))){
            if(!ss) {
                ss = new std::string;
                ss->append(str.c_str(), c - str.c_str());
            }
            //转化为ASCII码字符
            ss->append(1, (char)(xdigit_chars[(int)*(c + 1)] << 4 | xdigit_chars[(int)*(c + 2)]));
            c += 2;
        } else if(ss) {
            ss->append(1, *c);
        }
    }
    if(!ss) {
        return str;
    } else {
        std::string rt = *ss;
        delete ss;
        return rt;
    }
}

std::string StringUtil::Trim(const std::string& str, const std::string& delimit) {
    auto begin = str.find_first_not_of(delimit);
    if(begin == std::string::npos) {
        return "";
    }
    auto end = str.find_last_not_of(delimit);
    return str.substr(begin, end - begin + 1);
}

std::string StringUtil::TrimLeft(const std::string& str, const std::string& delimit) {
    auto begin = str.find_first_not_of(delimit);
    if(begin == std::string::npos) {
        return "";
    }
    return str.substr(begin);
}

std::string StringUtil::TrimRight(const std::string& str, const std::string& delimit) {
    auto end = str.find_last_not_of(delimit);
    if(end == std::string::npos) {
        return "";
    }
    return str.substr(0, end);
}

std::string StringUtil::WStringToString(const std::wstring& ws) {
    //对当前程序进行地域设置（本地设置、区域设置）,LC_ALL影响所有内容,使用当前操作系统的默认地域设置
    //如果 setlocale() 执行成功，那么返回一个指向字符串的指针，该字符串包含了当前地域设置的名称
    std::string str_locale = setlocale(LC_ALL, "");
    const wchar_t* wch_src = ws.c_str();
    //调用mbstowcs()函数，设置参数 wcstr 为NULL（用以获取转换所需的接收缓冲区大小）
    size_t n_dest_size = wcstombs(NULL, wch_src, 0) + 1;
    char *ch_dest = new char[n_dest_size];
    memset(ch_dest,0,n_dest_size);
    // size_t wcstombs(char *str, const wchar_t *pwcs, size_t n) 把宽字符字符串
    //pwcs 转换为一个 str 开始的多字节字符串。最多会有 n 个字节被写入 str 中。
    wcstombs(ch_dest,wch_src,n_dest_size);
    std::string str_result = ch_dest;
    delete []ch_dest;
    setlocale(LC_ALL, str_locale.c_str());
    return str_result;
}

std::wstring StringUtil::StringToWString(const std::string& s) {
    std::string str_locale = setlocale(LC_ALL, "");
    const char* chSrc = s.c_str();
    size_t n_dest_size = mbstowcs(NULL, chSrc, 0) + 1;
    wchar_t* wch_dest = new wchar_t[n_dest_size];
    wmemset(wch_dest, 0, n_dest_size);
    mbstowcs(wch_dest,chSrc,n_dest_size);
    std::wstring wstr_result = wch_dest;
    delete []wch_dest;
    setlocale(LC_ALL, str_locale.c_str());
    return wstr_result;
}

}
#ifndef __SYLAR_LOG_H__
#define __SYLAR_LOG_H__

#include <string>
#include <stdint.h>
#include <sstream>
#include <fstream>
#include <vector>
#include <list>
#include <stdarg.h>
#include <map>
#include <memory>
#include "singleton.h"
#include "util.h"

//使用流式方式将日志级别level的日志写入到logger
#define SYLAR_LOG_LEVEL(logger, level) \
    if(logger->getLevel() <= level) \
        sylar::LogEventWrap(sylar::LogEvent::ptr(new sylar::LogEvent(logger, level, \
                        __FILE__, __LINE__, 0, sylar::GetThreadId(),\
                sylar::GetFiberId(), time(0)))).getSS()

#define SYLAR_LOG_DEBUG(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::DEBUG)
#define SYLAR_LOG_INFO(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::INFO)
#define SYLAR_LOG_WARN(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::WARN)
#define SYLAR_LOG_ERROR(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::ERROR)
#define SYLAR_LOG_FATAL(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::FATAL)

#define SYLAR_LOG_FMT_LEVEL(logger, level, fmt, ...) \
    if(logger->getLevel() <= level) \
        sylar::LogEventWrap(sylar::LogEvent::ptr(new sylar::LogEvent(logger, level, \
                        __FILE__, __LINE__, 0, sylar::GetThreadId(),\
                sylar::GetFiberId(), time(0)))).getEvent()->format(fmt, __VA_ARGS__)

#define SYLAR_LOG_FMT_DEBUG(logger, fmt, ...) SYLAR_LOG_FMT_LEVEL(logger, sylar::LogLevel::DEBUG, fmt, __VA_ARGS__)
#define SYLAR_LOG_FMT_INFO(logger, fmt, ...)  SYLAR_LOG_FMT_LEVEL(logger, sylar::LogLevel::INFO, fmt, __VA_ARGS__)
#define SYLAR_LOG_FMT_WARN(logger, fmt, ...)  SYLAR_LOG_FMT_LEVEL(logger, sylar::LogLevel::WARN, fmt, __VA_ARGS__)
#define SYLAR_LOG_FMT_ERROR(logger, fmt, ...) SYLAR_LOG_FMT_LEVEL(logger, sylar::LogLevel::ERROR, fmt, __VA_ARGS__)
#define SYLAR_LOG_FMT_FATAL(logger, fmt, ...) SYLAR_LOG_FMT_LEVEL(logger, sylar::LogLevel::FATAL, fmt, __VA_ARGS__)

#define SYLAR_LOG_ROOT() sylar::LoggerMgr::GetInstance()->getRoot()

namespace sylar{

class Logger;
//日志级别
class LogLevel{
 public:
  //日志级别枚举
  enum Level{
    UNKNOW = 0,
    DEBUG = 1,
    INFO = 2,
    WARN = 3,
    ERROR = 4,
    FATAL = 5
  };
  //将日志级别转换成文本输出,静态方法不需要对象
  static const char* ToString(LogLevel::Level level);
  //将文本转换成日志级别
  static LogLevel::Level FromString(const std::string& str);


};

//日志事件
class LogEvent {
 public:
  typedef std::shared_ptr<LogEvent> ptr;
  LogEvent(std::shared_ptr<Logger> logger, LogLevel::Level level, const char* file, 
          int32_t line, uint32_t elapse, uint32_t thread_id, uint32_t fiber_t, 
          uint64_t time);
  //日志文件名
  const char* getFile() const { return m_file; }
  //返回行号
  int32_t getLine() const { return m_line; }
  //返回耗时
  uint32_t getElapse() const { return m_elapse; }
  //返回线程ID
  uint32_t getThreadId() const { return m_threadId; }
  //返回协程ID
  uint32_t getFiberId() const{ return m_fiberId; }
  //返回线程名称
  const std::string& getThredName() const { return m_threadName; }
  //返回日志内容
  std::string getContent() const { return m_ss.str();}
  //返回日志器
  std::shared_ptr<Logger> getLogger() const { return m_logger;}
  //返回日志级别
  LogLevel::Level getLevel() const { return m_level;}
  //返回日志内容字符串
  std::stringstream& getSS() { return m_ss;}
  uint64_t getTime() const { return m_time;}
  //格式化写入日志内容
  void format(const char* fmt, ...);
  //格式化写入日志内容
  void format(const char* fmt, va_list al);
 private:
  // 文件名
  const char* m_file = nullptr;
  // 行号
  int32_t m_line = 0;
  // 程序启动开始到现在的毫秒数
  uint32_t m_elapse = 0;
  // 线程ID
  uint32_t m_threadId = 0;
  // 协程ID
  uint32_t m_fiberId = 0;
  // 时间戳
  uint64_t m_time = 0;
  // 线程名称
  std::string m_threadName;
  // 日志内容流
  std::stringstream m_ss;
  // 日志器
  std::shared_ptr<Logger> m_logger;
  // 日志等级
  LogLevel::Level m_level;
};
//日志事件包装器
class LogEventWrap{
 public:
  //LogEvent::ptr LogEvent指针
  LogEventWrap(LogEvent::ptr e);
  ~LogEventWrap();
  //获取日志事件
  LogEvent::ptr getEvent() const { return m_event; }
  //获取日志内容流
  std::stringstream& getSS();
 private:
  LogEvent::ptr m_event;
};


class LogFormatter {
 public:
  typedef std::shared_ptr<LogFormatter> ptr;
  /**
     * @brief 构造函数
     * @param[in] pattern 格式模板
     * @details 
     *  %m 消息
     *  %p 日志级别
     *  %r 累计毫秒数
     *  %c 日志名称
     *  %t 线程id
     *  %n 换行
     *  %d 时间
     *  %f 文件名
     *  %l 行号
     *  %T 制表符
     *  %F 协程id
     *  %N 线程名称
     *
     *  默认格式 "%d{%Y-%m-%d %H:%M:%S}%T%t%T%N%T%F%T[%p]%T[%c]%T%f:%l%T%m%n"
     */
  LogFormatter(const std::string& pattern);
  std::string format(std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event);
  std::ostream& format(std::ostream& ofs, std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event);

  //日志内容项格式化
  class FormatItem {
   public:
    typedef std::shared_ptr<FormatItem> ptr;
    virtual ~FormatItem(){}
    virtual void format(std::ostream& os, std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event) = 0;
  };

  void init();
  bool isError() const { return m_error; };
  private:
  // 日志格式模板
  std::string m_pattern;
  // 日志格式解析后格式
  std::vector<FormatItem::ptr> m_items;
  // 是否有错误
  bool m_error = false;
};

//日志输出器父类
class LogAppender {
 public:
  typedef std::shared_ptr<LogAppender> ptr;
  virtual ~LogAppender() {}
  virtual void log(std::shared_ptr<Logger> logger, LogLevel::Level level, LogEvent::ptr event) = 0;

  void setFormatter(LogFormatter::ptr val) { m_formatter = val; }
  LogFormatter::ptr getFormatter() const { return m_formatter;}
  
  LogLevel::Level getLevel() const { return m_level;}
  void setLevel(LogLevel::Level val){  m_level = val;} 
protected:
  LogLevel::Level m_level = LogLevel::DEBUG;
  LogFormatter::ptr m_formatter;

};
//日志器
class Logger : public std::enable_shared_from_this<Logger>{
 public:
  typedef std::shared_ptr<Logger> ptr;

  Logger(const std::string& name = "root");
  void log(LogLevel::Level level, LogEvent::ptr event);

  void debug(LogEvent::ptr event);
  void info(LogEvent::ptr event);
  void warn(LogEvent::ptr event);
  void error(LogEvent::ptr event);
  void fatal(LogEvent::ptr event);

  void addAppender(LogAppender::ptr appender);
  void delAppender(LogAppender::ptr appender);
  LogLevel::Level getLevel() const { return m_level;}
  void setLevel(LogLevel::Level val) { m_level = val;}
  const std::string& getName() const { return m_name;}

private:
    std::string m_name;                     //日志名称
    LogLevel::Level m_level;                //日志级别怎么什么类都有
    std::list<LogAppender::ptr> m_appenders;//Appender集合，为甚么是集合
    LogFormatter::ptr m_formatter;

};

class StdoutLogAppender : public LogAppender {
 public:
  typedef std::shared_ptr<StdoutLogAppender> ptr;
  void log(Logger::ptr logger, LogLevel::Level level, LogEvent::ptr event) override;

};
//定义输出到文件的Appender
class FileLogAppender : public LogAppender {
public:
    typedef std::shared_ptr<FileLogAppender> ptr;
    FileLogAppender(const std::string& filename);
    void log(Logger::ptr logger, LogLevel::Level level, LogEvent::ptr event) override;

    //重新打开文件，文件打开成功返回true
    bool reopen();
private:
    std::string m_filename;
    std::ofstream m_filestream;
};

class LoggerManager {
public:
    LoggerManager();
    Logger::ptr getLogger(const std::string& name);

    void init();
    Logger::ptr getRoot() const { return m_root;}
private:
    std::map<std::string, Logger::ptr> m_loggers;//名字和具体的日志器，日志器处理事件和日志具体打印地址
    Logger::ptr m_root;
};

typedef sylar::Singleton<LoggerManager> LoggerMgr;//全局静态的单例管理器
}
#endif

#include<arpa/inet.h>
#include<fcntl.h>
#include<sys/epoll.h>
#include<sys/socket.h>
#include<sys/types.h>
#include<fmt/format.h>
#include<netdb.h>
#include<unistd.h>
#include<string.h>
#include <thread>
#include <vector>
#include <algorithm>
#include <map>
#include <cassert>
int check_error(const char *msg, int res){
    if(res == -1){
        fmt::println("{}: {}",msg, gai_strerror(errno));
        throw;
    }
    return res;
}

size_t check_error(const char *msg, ssize_t res){
    if(res == -1){
        fmt::println("{}: {}",msg, gai_strerror(errno));
        throw;
    }
    return res;
}

//
#define CHECK_CALL(func, ...) check_error(#func, func(__VA_ARGS__));

//封装胖指针
struct socket_address_fatptr {
    struct sockaddr *m_addr;
    socklen_t m_addrlen;
};

struct socket_address_storage{
    union {
        struct sockaddr m_addr;
        struct sockaddr_storage m_addr_storage;
    };
    socklen_t m_addrlen = sizeof(struct sockaddr_storage);

    operator socket_address_fatptr(){
        return {&m_addr, m_addrlen};
    }
};

struct address_resolved_entry{
    struct addrinfo *m_curr = nullptr;
    socket_address_fatptr get_address() const {
        return {m_curr->ai_addr, m_curr->ai_addrlen};
    }
    int create_socket() const {
        // 这样写有点不方便，用宏来封装
        // int sockfd = check_error("socket", socket(m_curr->ai_family, m_curr->ai_socktype, m_curr->ai_protocol));
        int sockfd = CHECK_CALL(socket, m_curr->ai_family, m_curr->ai_socktype, m_curr->ai_protocol);
        return sockfd;
    }

    int create_socket_and_bind() const{
        int sockfd = create_socket();
        socket_address_fatptr serve_addr = get_address();   //在entry对象还活着的时候取出来
        CHECK_CALL(bind, sockfd, serve_addr.m_addr, serve_addr.m_addrlen);
        return sockfd;
    }
    [[nodiscared]] bool next_entry(){
        m_curr = m_curr->ai_next;
        if(m_curr == nullptr){
            return false;
        }
        return true;
    }
};
struct address_resolver {
    struct addrinfo *m_head = nullptr;
    void resolve(std::string const &name, std::string const &service){
        int err = getaddrinfo(name.c_str(), service.c_str(), NULL, &m_head);
        if(err != 0){
            fmt::println("getaddrinfo: {}, {}",gai_strerror(err), err);
            throw;
        }
    }
    address_resolved_entry get_first_entry(){
        return {m_head};
    }
    address_resolver() = default;
    address_resolver(address_resolver &&that) : m_head(that.m_head){
        that.m_head = nullptr;
    }
    ~address_resolver() {
        if(m_head){
            freeaddrinfo(m_head);
        }
    }
};

using StringMap = std::map<std::string, std::string>;

struct http11_request_parser {
    std::string m_header;
    std::string m_heading_line;
    StringMap m_header_keys;
    std::string m_body;
    bool m_header_finished = false;
    //正文结束不需要更多字节
    [[nodiscared]] bool header_finished() {
        return m_header_finished;
    }
    
    void _extract_headers(){
        size_t pos = m_header.find("\r\n");
        m_heading_line = m_header.substr(0, pos);
        while(pos != std::string::npos){
            //跳过\r\n
            pos += 2;
            //从当前pos往下找
            size_t next_pos = m_header.find("\r\n", pos);
            size_t line_len = std::string::npos;
            //如果下一行不是结束，line_len获取当前行到下一行的距离
            if(next_pos != std::string::npos){
                line_len = next_pos - pos;
            }
            std::string_view line = std::string_view(m_header).substr(pos, line_len);
            size_t colon = line.find(": ");
            if(colon != std::string::npos){
                std::string key = std::string(line.substr(0, colon));
                //排除": ",注意这里是两个字符
                std::string_view value = line.substr(colon + 2);
                //转换成小写
                std::transform(key.begin(), key.end(), key.begin(), [] (char c){
                    if('A' <= c && c <= 'Z'){
                        c += 'a' - 'A';
                    }
                    return c;
                });
                // if(key == "content_length"){
                //     content_length = std::stoi(value);
                // }
                m_header_keys.insert_or_assign(std::move(key), value);
            }
            pos = next_pos;
        }
    }
    
    void push_chunk(std::string_view chunk){
        assert(!m_header_finished);
        m_header.append(chunk);
        size_t header_len = m_header.find("\r\n\r\n");
            //头部已经结束
        if(header_len != std::string::npos){
            m_header_finished = true;
            m_body = m_header.substr(header_len + 4);
            m_header.resize(header_len);
            //解析头部中的Content_length字段
            //http响应不区分大小写，在解析Content_length的时候不能直接find
            _extract_headers();
        }
    }

    std::string &headline() {
        return m_heading_line;
    }
    std::string &headers_raw() {
        return m_header;
    }
    StringMap &headers() {
        return m_header_keys;
    }
    std::string &extra_body() {
        return m_body;
    }
    
};

template<class HeaderParser = http11_request_parser>
struct _http_base_parser {
    HeaderParser m_header_parser;
    size_t m_content_length = 0;
    bool m_body_finished = false;
    //正文结束不需要更多字节
    [[nodiscared]] bool request_finished() {
        return m_body_finished;
    }

    
    
    std::string &m_header_raw() {
        return m_header_parser.headers_raw();
    }

    std::string &headline() {
        return m_header_parser.headline();
    }

    StringMap &headers() {
        return m_header_parser.headers();
    }

    std::string &body() {
        return m_header_parser.extra_body();
    }

    std::string _headline_first() {
        auto &line = headline();
        size_t space = line.find(' ');
        if(space == std::string::npos){
            return "";
        }
        return line.substr(0, space);
    }

    std::string _headline_second(){
        auto &line = headline();
        size_t space1 = line.find(' ');
        if(space1 == std::string::npos){
            return "GET";
        }
        size_t space2 = line.find(' ', space1 + 1);
        if(space2 == std::string::npos){
            return "GET";
        }
        return line.substr(space1, space2 - space1);
    }

    std::string _headline_third(){
        auto &line = headline();
        size_t space1 = line.find(' ');
        if(space1 == std::string::npos){
            return "GET";
        }
        size_t space2 = line.find(' ', space1 + 1);
        if(space2 == std::string::npos){
            return "GET";
        }
        return line.substr(space2 + 1);
    }

    size_t _extract_content_length(){
        auto& headers = m_header_parser.headers();
        auto it = headers.find("content-length");
        if(it == headers.end()){
            return 0;
        }
        try{
            return std::stoi(it->second);
        }catch (std::invalid_argument const &){
            return 0;
        }
    }
    void push_chunk(std::string_view chunk){
        if(!m_header_parser.header_finished()){
            m_header_parser.push_chunk(chunk);
            if(m_header_parser.header_finished()){
                m_content_length = _extract_content_length();
                if(body().size() >= m_content_length){
                    m_body_finished = true;
                    body().resize(m_content_length);
                }
            }
        }
        else{
            //正文已经结束但是收到了正文的其他部分
            body().append(chunk);
            if(body().size() >= m_content_length){
                m_body_finished = true;
                body().resize(m_content_length);
            }
        }
    }
};

template<class HeaderParser = http11_request_parser>
struct http_request_parser : _http_base_parser<HeaderParser>{
    std::string method(){
        return this->_headline_first();
    }

    std::string url() {
        return this->_headline_second();
    }

    std::string http_version() {
        return this->_headline_third();
    }
};

template<class HeaderParser = http11_request_parser>
struct http_response_parser : _http_base_parser<HeaderParser>{
    std::string http_version(){
        return this->_headline_first();
    }

    int status() {
        auto s = this->_headline_second();
        try{
            return std::stoi(s);
        }catch (std::logic_error const &) {
            return -1;
        }
    }

    std::string status_string() {
        return this->_headline_third();
    }
};

//构造响应
struct http_response_writer{
    std::string header = "";
    std::string buffer(){
        return header;
    }
    void begin_header(int status){
        header.append("HTTP/1.1 " + std::to_string(status) + " OK" + "\r\n");
    }
    void write_header(std::string a, std::string b){
        std::string temp = a + ": " + b + "\r\n";
        header.append(temp);
    }
    void end_header(){
        header.append("\r\n");
    }
};



//简单线程池
std::vector<std::thread> pool;
int main(){
    setlocale(LC_ALL, "zh_CN");
    address_resolver resolver;
    resolver.resolve("127.0.0.1", "8080");
    fmt::println("正在监听127.0.0.1:8080");
    auto entry = resolver.get_first_entry();
    int listenfd = entry.create_socket_and_bind();
    CHECK_CALL(listen, listenfd, SOMAXCONN);
    socket_address_storage addr;
    while(true){
        int connid = CHECK_CALL(accept, listenfd, &addr.m_addr, &addr.m_addrlen);
        fmt::println("接受连接：{}", connid);
        //detach可能导致内存泄漏
        //线程里最好用值捕获，visit就地调用可以用引用捕获
        //显式构造thread并放入线程池
        pool.emplace_back([connid] {
            while(true){
                char buf[1024];
                http_request_parser req_parse;
                do{
                    size_t n = CHECK_CALL(read, connid, buf, sizeof(buf));
                    //n为0表示读完了，还需要判断n为负数(出错)的情况
                    if(n == 0){
                        fmt::println("读到末尾，对面关闭连接");
                        return;
                    }
                    req_parse.push_chunk(std::string_view(buf, n)); 
                }while(!req_parse.request_finished());
                // auto req = req_parse.m_header; //不需要判断字符串尾部是否为\0
                // fmt::println("request: {}", req);
                fmt::println("收到请求：{}", connid);
                // fmt::println("收到请求头： {}", req_parse.m_header_raw());
                // fmt::println("收到请求正文：{}", req_parse.body());
                // std::string res = "你好！" + req;
                std::string body = req_parse.body();
                // fmt::println("{}, {}, {}", req_parse.method(), req_parse.url(), req_parse.http_version());
                // 构造响应

                if(body.empty()){
                    body = "你好，你的请求正文为空！\r\n";
                }else {
                    body = "你好，你的请求是：[" + body + "]";
                }
                http_response_writer res_writer;
                res_writer.begin_header(200);
                res_writer.write_header("Server", "ChatServer");
                res_writer.write_header("Content-Type", "text/html;charset=utf-8");
                res_writer.write_header("Connection", "keep-alive");
                res_writer.write_header("Content-Length", std::to_string(body.size()));
                res_writer.end_header();
                //保证效率，组完头部就直接写
                auto buffer = res_writer.buffer();
                fmt::println("正在响应：{}", connid);
                CHECK_CALL(write, connid, buffer.data(), buffer.size());
                CHECK_CALL(write, connid, body.data(), body.size());
                fmt::println("我的响应头：{}", buffer);
                fmt::println("我的响应正文：{}", body);
            }
            fmt::println("连接结束: {}", connid);
            close(connid);
        });
    }
    //对线程池里所有线程进行join
    for(auto &t : pool)
        {t.join();}
    return 0;
}
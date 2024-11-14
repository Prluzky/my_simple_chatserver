#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <deque>
#include <stdexcept>
#include <system_error>
#include <functional>
#include <fcntl.h>
#include <map>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <fmt/format.h>
#include <string.h>
#include <utility>
#include <vector>

struct no_move {
    no_move() = default;
    no_move(no_move &&) = delete;
    no_move &operator=(no_move &&) = delete;
    no_move(no_move const &) = delete;
    no_move &operator=(no_move const &) = delete;
};

std::error_category const &gai_category() {
    // struct gai_category : std::error_category{
    //     char const *name() const noexcept override {
    //         return "getaddrinfo";
    //     }
    //     std::string message(int err) const override{
    //         return gai_strerror(err);
    //     }
    // };
    // static gai_category instance;
    //简写
    static struct gai_category final : std::error_category{
        char const *name() const noexcept override {
            return "getaddrinfo";
        }
        std::string message(int err) const override{
            return gai_strerror(err);
        }
    } instance;
    return instance;
}


[[noreturn]] void _throw_system_error(const char *what) {
    auto ec = std::error_code(errno, std::system_category());
    fmt::println(stderr, "{}: {} ({}.{})", what, ec.message(), ec.category().name(), ec.value());
    throw std::system_error(ec, what);
}

template <int Except = 0, class T>
T check_error(const char *what, T res){
    if(res == -1){
        if constexpr (Except != 0){
            if(errno == Except){
                return -1;
            }
        }
        _throw_system_error(what);
    }
    return res;
}
//

// #define SOURCE_INFO_IMPL_2(file, line) "In " file ":" #line ": "
// #define SOURCE_INFO_IMPL(file, line) SOURCE_INFO_IMPL_2(file, line)
#define SOURCE_INFO_IMPL(file, line) "In " file ":" #line ": "
#define SOURCE_INFO() SOURCE_INFO_IMPL(__FILE__, __LINE__)
#define CHECK_CALL_EXCEPT(except, func, ...) check_error<except>(SOURCE_INFO() #func, func(__VA_ARGS__))
#define CHECK_CALL(func, ...) check_error(SOURCE_INFO() #func, func(__VA_ARGS__))

struct address_resolver {

    //封装胖指针
    struct address_ref {
        struct sockaddr *m_addr;
        socklen_t m_addrlen;
    };

    struct address {
        union {
            struct sockaddr m_addr;
            struct sockaddr_storage m_addr_storage;
        };
        socklen_t m_addrlen = sizeof(struct sockaddr_storage);
        operator address_ref() {
            return {&m_addr, m_addrlen};
        }
    };

    struct address_info{
        struct addrinfo *m_curr = nullptr;
        address_ref get_address() const {
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
            address_ref serve_addr = get_address();   //在entry对象还活着的时候取出来
            int on = 1;
            setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
            setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
            CHECK_CALL(bind, sockfd, serve_addr.m_addr, serve_addr.m_addrlen);
            CHECK_CALL(listen, sockfd, SOMAXCONN);
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

    struct addrinfo *m_head = nullptr;
    //todo，原项目是addrinfo
    address_info resolve(std::string const &name, std::string const &service){
        int err = getaddrinfo(name.c_str(), service.c_str(), NULL, &m_head);
        if(err != 0){
            fmt::println("getaddrinfo: {}, {}",gai_strerror(err), err);
            auto ec = std::error_code(err, gai_category());
            throw std::system_error(ec, name + ": " + service);
        }
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

using string_map = std::map<std::string, std::string>;

struct bytes_const_view {
    char const *m_data;
    size_t m_size;

    char const *data() const noexcept {
        return m_data;
    }

    size_t size() const noexcept {
        return m_size;
    }

    char const *begin() const noexcept {
        return data();
    }

    char const *end() const noexcept {
        return data() + size();
    }

    bytes_const_view subspan(size_t start, size_t len = static_cast<size_t>(-1)) const {
        if (start > size())
            throw std::out_of_range("bytes_const_view::subspan");
        if (len > size() - start)
            len = size() - start;
        return {data() + start, len};
    }
    operator std::string_view() const noexcept {
        return std::string_view{data(), size()};
    }
};

struct bytes_view {
    char *m_data;
    size_t m_size;

    char *data() const noexcept {
        return m_data;
    }

    size_t size() const noexcept {
        return m_size;
    }

    char *begin() const noexcept {
        return data();
    }

    char *end() const noexcept {
        return data() + size();
    }

    bytes_view subspan(size_t start, size_t len) const {
        if (start > size())
            throw std::out_of_range("bytes_view::subspan");
        if (len > size() - start)
            len = size() - start;
        return {data() + start, len};
    }
    operator bytes_const_view() const noexcept {
        return bytes_const_view{data(), size()};
    }

    operator std::string_view() const noexcept {
        return std::string_view{data(), size()};
    }
};

struct bytes_buffer {
    std::vector<char> m_data;

    bytes_buffer() = default;
    bytes_buffer(bytes_buffer &&) = default;
    bytes_buffer &operator=(bytes_buffer &&) = default;
    explicit bytes_buffer(bytes_buffer const &) = default;
    explicit bytes_buffer(size_t n) : m_data(n) {}

    char const *data() const noexcept {
        return m_data.data();
    }

    char *data() noexcept {
        return m_data.data();
    }

    size_t size() const noexcept {
        return m_data.size();
    }

    char const *begin() const noexcept {
        return data();
    }

    char *begin() noexcept {
        return data();
    }

    char const *end() const noexcept {
        return data() + size();
    }

    char *end() noexcept {
        return data() + size();
    }

    bytes_const_view subspan(size_t start, size_t len) const {
        return operator bytes_const_view().subspan(start, len);
    }
    bytes_view subspan(size_t start, size_t len) {
        return operator bytes_view().subspan(start, len);
    }
    operator bytes_const_view() const noexcept {
        return bytes_const_view{m_data.data(), m_data.size()};
    }

    operator bytes_view() noexcept {
        return bytes_view{m_data.data(), m_data.size()};
    }

    operator std::string_view() const noexcept {
        return std::string_view{m_data.data(), m_data.size()};
    }

    void append(bytes_const_view chunk) {
        m_data.insert(m_data.end(), chunk.begin(), chunk.end());
    }

    void append(std::string_view chunk) {
        m_data.insert(m_data.end(), chunk.begin(), chunk.end());
    }
    template <size_t N>
    void append_literial(const char (&literial)[N]) {
        append(std::string_view{literial, N - 1});
    }

    void clear() {
        m_data.clear();
    }

    void resize(size_t n) {
        m_data.resize(n);
    }

    void reserve(size_t n) {
        m_data.reserve(n);
    }
};

template <size_t N>
struct static_bytes_buffer {
    std::array<char, N> m_data;

    char const *data() const noexcept {
        return m_data.data();
    }

    char *data() noexcept {
        return m_data.data();
    }

    static constexpr size_t size() noexcept {
        return N;
    }

    operator bytes_const_view() const noexcept {
        return bytes_const_view{m_data.data(), N};
    }

    operator bytes_view() noexcept {
        return bytes_view{m_data.data(), N};
    }

    operator std::string_view() const noexcept {
        return std::string_view{m_data.data(), m_data.size()};
    }
};

struct http11_request_parser {
    bytes_buffer m_header;
    std::string m_heading_line;
    string_map m_header_keys;
    std::string m_body;
    bool m_header_finished = false;
    //正文结束不需要更多字节
    [[nodiscared]] bool header_finished() {
        return m_header_finished;
    }
    
    void _extract_headers(){
        std::string_view header = m_header;
        size_t pos = header.find("\r\n");
        m_heading_line = header.substr(0, pos);
        // size_t pos = header.find("\r\n", 0, 2);
        while(pos != std::string::npos){
            //跳过\r\n
            pos += 2;
            //从当前pos往下找
            size_t next_pos = header.find("\r\n", pos);
            // size_t next_pos = header.find("\r\n", pos, 2);
            size_t line_len = std::string::npos;
            //如果下一行不是结束，line_len获取当前行到下一行的距离
            if(next_pos != std::string::npos){
                line_len = next_pos - pos;
            }
            std::string_view line = header.substr(pos, line_len);
            size_t colon = line.find(": ");
            // size_t colon = line.find(": ", 0, 2);
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
    
    void push_chunk(bytes_const_view chunk){
        assert(!m_header_finished);
        size_t old_size = m_header.size();
        m_header.append(chunk);
        std::string_view header = m_header;
        // size_t header_len = header.find("\r\n\r\n");
        if (old_size < 4)
            old_size = 4;
        old_size -= 4;
        size_t header_len = header.find("\r\n\r\n", old_size, 4);
        if(header_len != std::string::npos){
            m_header_finished = true;
            m_body = header.substr(header_len + 4);
            m_header.resize(header_len);
            //解析头部中的Content_length字段
            //http响应不区分大小写，在解析Content_length的时候不能直接find
            _extract_headers();
        }
    }

    std::string &headline() {
        return m_heading_line;
    }
    
    string_map &headers() {
        return m_header_keys;
    }
    bytes_buffer &headers_raw() {
        return m_header;
    }

    std::string &extra_body() {
        return m_body;
    }
    
};

template<class HeaderParser = http11_request_parser>
struct _http_base_parser {
    HeaderParser m_header_parser;
    size_t m_content_length = 0;
    size_t body_accumulated_size = 0;
    bool m_body_finished = false;
    //正文结束不需要更多字节

    void reset_state() {
        m_header_parser.reset_state();
        m_content_length = 0;
        body_accumulated_size = 0;
        m_body_finished = false;
    }

    [[nodiscard]] bool header_finished() {
        return m_header_parser.header_finished();
    }

    [[nodiscared]] bool request_finished() {
        return m_body_finished;
    }

    std::string &m_header_raw() {
        return m_header_parser.headers_raw();
    }

    std::string &headline() {
        return m_header_parser.headline();
    }

    string_map &headers() {
        return m_header_parser.headers();
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
            return "";
        }
        size_t space2 = line.find(' ', space1 + 1);
        if(space2 == std::string::npos){
            return "";
        }
        return line.substr(space1, space2 - space1);
    }

    std::string _headline_third(){
        auto &line = headline();
        size_t space1 = line.find(' ');
        if(space1 == std::string::npos){
            return "";
        }
        size_t space2 = line.find(' ', space1 + 1);
        if(space2 == std::string::npos){
            return "";
        }
        return line.substr(space2 + 1);
    }

    std::string &body() {
        return m_header_parser.extra_body();
    }

    size_t _extract_content_length(){
        auto& headers = m_header_parser.headers();
        auto it = headers.find("content-length");
        if(it == headers.end()){
            return 0;
        }
        try{
            return std::stoi(it->second);
        }catch (std::logic_error const &){
            return 0;
        }
    }
    
    void push_chunk(bytes_const_view chunk){
        assert(!m_body_finished);
        if(!m_header_parser.header_finished()){
            m_header_parser.push_chunk(chunk);
            if(m_header_parser.header_finished()){
                body_accumulated_size = body().size();
                m_content_length = _extract_content_length();
                if(body_accumulated_size >= m_content_length){
                    m_body_finished = true;
                    // body().resize(m_content_length);
                }
            }
        }
        else{
            //正文已经结束但是收到了正文的其他部分
            body().append(chunk);
            body_accumulated_size += chunk.size();
            if(body_accumulated_size >= m_content_length){
                m_body_finished = true;
            }
        }
    }

    std::string read_some_body() {
        return std::move(body());
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
struct http11_head_writer{
    bytes_buffer m_buffer;

    void reset_state() {
        m_buffer.clear();
    }

    bytes_buffer &buffer() {
        return m_buffer;
    }
     void begin_header(std::string_view first, std::string_view second, std::string_view third) {
        m_buffer.append(first);
        m_buffer.append_literial(" ");
        m_buffer.append(second);
        m_buffer.append_literial(" ");
        m_buffer.append(third);
    }

    void write_header(std::string_view key, std::string_view value) {
        m_buffer.append_literial("\r\n");
        m_buffer.append(key);
        m_buffer.append_literial(": ");
        m_buffer.append(value);
    }

    void end_header() {
        m_buffer.append_literial("\r\n\r\n");
    }
};


template<class HeaderWriter = http11_head_writer>
struct http_response_writer {
    HeaderWriter m_header_writer;
    bytes_buffer &buffer() {
        return m_header_writer.buffer();
    }

    void begin_header(int status) {
        m_header_writer.begin_header("HTTP/1.1", std::to_string(status), "OK");
    }

    void write_header(std::string_view key, std::string_view value) {
        m_header_writer.write_header(key, value);
    }

    void end_header(){
        m_header_writer.end_header();
    }
};

//简单线程池
std::vector<std::thread> pool;

void server() {
    address_resolver resolver;
    fmt::println("正在监听：127.0.0.1:8080");
    auto entry = resolver.resolve("127.0.0.1", "8080");
    int listenfd = entry.create_socket_and_bind();
    // CHECK_CALL(listen, listenfd, SOMAXCONN);
    // address_resolver::socket_address_storage addr;
    while(true){
        address_resolver::address addr;
        int connid = CHECK_CALL(accept, listenfd, &addr.m_addr, &addr.m_addrlen);
        fmt::println("接受了一个连接: {}", connid);
        //detach可能导致内存泄漏
        //线程里最好用值捕获，visit就地调用可以用引用捕获
        //显式构造thread并放入线程池
        pool.emplace_back([connid] {
                bytes_buffer buf(1024);
                while(true){
                    http_request_parser req_parse;
                    fmt::println("开始读取...");
                    do {
                        // 注意：TCP 基于流，可能粘包
                        size_t n = CHECK_CALL(read, connid, buf.data(), buf.size());
                        // 如果读到 EOF，说明对面，关闭了连接
                        if (n == 0) {
                            fmt::println("收到对面关闭了连接: {}", connid);
                            goto quit;
                        }
                        fmt::println("读取到了 {} 字节: {}", n, std::string_view{buf.data(), n});
                        // 成功读取，则推入解析
                        req_parse.push_chunk(buf.subspan(0, n));
                    } while (!req_parse.request_finished());
                    fmt::println("收到请求: {}", connid);
                    // fmt::println("请求头: {}", req_parse.headers_raw());
                    // fmt::println("请求正文: {}", req_parse.body());
                    std::string body = req_parse.body();
                    if (body.empty()) {
                        body = "你好，你的请求正文为空哦";
                    } else {
                        body = fmt::format("你好，你的请求是: [{}]，共 {} 字节", body, body.size());
                    }
                    http_response_writer res_writer;
                    res_writer.begin_header(200);
                    res_writer.write_header("Server", "co_http");
                    res_writer.write_header("Content-type", "text/html;charset=utf-8");
                    res_writer.write_header("Connection", "keep-alive");
                    res_writer.write_header("Content-length", std::to_string(body.size()));
                    res_writer.end_header();
                    auto &buffer = res_writer.buffer();
                    CHECK_CALL(write, connid, buffer.data(), buffer.size());
                    CHECK_CALL(write, connid, body.data(), body.size());
                    // fmt::println("我的响应头: {}", buffer);
                    // fmt::println("我的响应正文: {}", body);
                    fmt::println("正在响应: {}", connid);
                }
            quit:
                fmt::println("连接结束: {}", connid);
                close(connid);
        });
    }
}

int main(){
    setlocale(LC_ALL, "zh_CN.UTF-8");
    try{
        server();
    }catch (std::exception const &e){
        fmt::println("错误： {}", e.what());
    }
    for(auto &t : pool)
        {t.join();}
    return 0;
}
// Application worker implementation for the HTTP plugin.
// Performs blocking network I/O on a dedicated thread and reports results via
// a thread-safe queue to be drained on the engine thread.
#include "Http.hpp"
#include <regex>
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
#include <openssl/x509v3.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#endif

#if defined(_WIN32)
namespace
{
    std::wstring utf8ToWide (const std::string& value)
    {
        if(value.empty()) return {};
        const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), wide.data(), length);
        return wide;
    }

    bool performWindowsHttps (Http::Job& job, const std::string& host, int port, const std::string& path)
    {
        const wchar_t* method = nullptr;
        switch(job.method)
        {
            case Http::Method::Get: method = L"GET"; break;
            case Http::Method::Post: method = L"POST"; break;
            case Http::Method::Put: method = L"PUT"; break;
            case Http::Method::Patch: method = L"PATCH"; break;
            case Http::Method::Delete: method = L"DELETE"; break;
        }

        HINTERNET session = WinHttpOpen(L"Deepfield/0.7", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if(!session)
        {
            job.errorMessage = "WinHTTP session failed: " + std::to_string(GetLastError());
            return false;
        }
        WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);

        const std::wstring wideHost = utf8ToWide(host);
        HINTERNET connection = WinHttpConnect(session, wideHost.c_str(), static_cast<INTERNET_PORT>(port), 0);
        if(!connection)
        {
            job.errorMessage = "WinHTTP connection failed: " + std::to_string(GetLastError());
            WinHttpCloseHandle(session);
            return false;
        }

        const std::wstring widePath = utf8ToWide(path);
        HINTERNET request = WinHttpOpenRequest(connection, method, widePath.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if(!request)
        {
            job.errorMessage = "WinHTTP request failed: " + std::to_string(GetLastError());
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return false;
        }

        std::wstring headers;
        for(const auto& header : job.headers)
        {
            headers += utf8ToWide(header.first) + L": " + utf8ToWide(header.second) + L"\r\n";
        }
        const wchar_t* headerData = headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str();
        const DWORD headerLength = headers.empty() ? 0 : static_cast<DWORD>(headers.size());
        void* bodyData = job.body.empty() ? WINHTTP_NO_REQUEST_DATA : job.body.data();
        const DWORD bodyLength = static_cast<DWORD>(job.body.size());
        if(!WinHttpSendRequest(request, headerData, headerLength, bodyData, bodyLength, bodyLength, 0)
           || !WinHttpReceiveResponse(request, nullptr))
        {
            job.errorMessage = "WinHTTP send failed: " + std::to_string(GetLastError());
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return false;
        }

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if(!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
        {
            job.errorMessage = "WinHTTP status failed: " + std::to_string(GetLastError());
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return false;
        }

        std::string body;
        while(true)
        {
            DWORD available = 0;
            if(!WinHttpQueryDataAvailable(request, &available))
            {
                job.errorMessage = "WinHTTP read failed: " + std::to_string(GetLastError());
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                return false;
            }
            if(available == 0) break;

            const std::size_t offset = body.size();
            body.resize(offset + available);
            DWORD read = 0;
            if(!WinHttpReadData(request, body.data() + offset, available, &read))
            {
                job.errorMessage = "WinHTTP read failed: " + std::to_string(GetLastError());
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                return false;
            }
            body.resize(offset + read);
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        job.succeed = true;
        job.status = static_cast<int>(status);
        job.body = std::move(body);
        job.bodyBinary.assign(job.body.begin(), job.body.end());
        return true;
    }
}
#endif

Http::Http () : jobSignal(0)
{
    this->running = true;
    this->workThread = std::thread(&Http::worker, this);
}

Http::~Http ()
{
    this->running = false;
    this->jobSignal.release();
    this->workThread.join();
}

// Worker thread: waits for jobs and performs blocking HTTP/S requests.
void Http::worker ()
{
    while(this->running)
    {
        if(this->waiting.size() == 0) this->jobSignal.acquire();

        if(this->waiting.size() == 0) continue;

        Job job = this->waiting.frontPop();
        if(job.multipart)
        {
            std::size_t bodySize = 0;
            for(const auto& part : *job.multipart) bodySize += part.body.size() + 256;
            job.body.clear();
            job.body.reserve(bodySize);
            for(const auto& part : *job.multipart)
            {
                job.body += "--" + job.multipartBoundary + "\r\n";
                job.body += "Content-Disposition: form-data; name=\"" + part.name + "\"";
                if(!part.filename.empty()) job.body += "; filename=\"" + part.filename + "\"";
                job.body += "\r\n";
                if(!part.contentType.empty()) job.body += "Content-Type: " + part.contentType + "\r\n";
                job.body += "\r\n";
                job.body.append(part.body);
                job.body += "\r\n";
            }
            job.body += "--" + job.multipartBoundary + "--\r\n";
            job.multipart.reset();
        }

        std::regex urlPattern(R"(^(https?):\/\/([^\/:]+)(?::([0-9]+))?(\/\S*)?$)");
        std::smatch matches;

        if(!std::regex_search(job.url, matches, urlPattern))
        {
            job.succeed = false;
            job.errorMessage = "Invalid URL format";
            this->done.push(job);
            continue;
        }

        httplib::Result res;
        std::string contentType = "application/json";
        const auto contentTypeHeader = job.headers.find("Content-Type");
        if(contentTypeHeader != job.headers.end()) contentType = contentTypeHeader->second;

        const std::string scheme = matches[1].str();
        const std::string host = matches[2].str();
        const int port = matches[3].matched
            ? std::stoi(matches[3].str())
            : (scheme == "https" ? 443 : 80);
        const std::string path = matches[4].matched ? matches[4].str() : "/";

        if(scheme == "https")
        {
#if defined(_WIN32)
            performWindowsHttps(job, host, port, path);
            this->done.push(job);
            std::this_thread::yield();
            continue;
#elif defined(CPPHTTPLIB_OPENSSL_SUPPORT)
            httplib::SSLClient cli(host, port);
            cli.set_connection_timeout(10, 0);
            cli.set_read_timeout(15, 0);
            cli.set_write_timeout(15, 0);

            // Execute the appropriate HTTP method
            switch(job.method)
            {
                case Method::Get:
                    res = cli.Get(path, job.headers);
                    break;
                case Method::Post:
                    res = cli.Post(path, job.headers, job.body, contentType);
                    break;
                case Method::Put:
                    res = cli.Put(path, job.headers, job.body, contentType);
                    break;
                case Method::Patch:
                    res = cli.Patch(path, job.headers, job.body, contentType);
                    break;
                case Method::Delete:
                    res = cli.Delete(path, job.headers);
                    break;
                default:
                    job.succeed = false;
                    job.errorMessage = "Unsupported HTTP method";
                    this->done.push(job);
                    continue;
            }

            if(!res)
            {
                std::string errorStr = httplib::to_string(res.error());
                std::cout << "error code: " << res.error() << std::endl;
                job.errorMessage = errorStr;
                auto result = cli.get_openssl_verify_result();
                if (result)
                {
                    std::string sslError = X509_verify_cert_error_string(result);
                    std::cout << "verify error: " << sslError << std::endl;
                    job.errorMessage += " (SSL: " + sslError + ")";
                }
            }
#else
            job.succeed = false;
            job.errorMessage = "HTTPS is not supported by this build";
            this->done.push(job);
            continue;
#endif
        }
        else if(scheme == "http")
        {
            httplib::Client cli(host, port);
            cli.set_connection_timeout(10, 0);
            cli.set_read_timeout(15, 0);
            cli.set_write_timeout(15, 0);

            // Execute the appropriate HTTP method
            switch(job.method)
            {
                case Method::Get:
                    res = cli.Get(path, job.headers);
                    break;
                case Method::Post:
                    res = cli.Post(path, job.headers, job.body, contentType);
                    break;
                case Method::Put:
                    res = cli.Put(path, job.headers, job.body, contentType);
                    break;
                case Method::Patch:
                    res = cli.Patch(path, job.headers, job.body, contentType);
                    break;
                case Method::Delete:
                    res = cli.Delete(path, job.headers);
                    break;
                default:
                    job.succeed = false;
                    job.errorMessage = "Unsupported HTTP method";
                    this->done.push(job);
                    continue;
            }

            if(!res)
            {
                std::string errorStr = httplib::to_string(res.error());
                std::cout << "error code: " << res.error() << std::endl;
                job.errorMessage = errorStr;
            }
        }
        else
        {
            job.succeed = false;
            job.errorMessage = "Unsupported URL scheme (only http:// and https:// are supported)";
            this->done.push(job);
            continue;
        }

        if(res)
        {
            job.succeed = true;
            job.headers = res->headers;
            job.status = res->status;
            job.body = res->body;
            // Store binary data as well
            job.bodyBinary.assign(res->body.begin(), res->body.end());
        }
        else
        {
            job.succeed = false;
            if(job.errorMessage.empty())
            {
                job.errorMessage = "HTTP request failed";
            }
        }

        this->done.push(job);

        std::this_thread::yield();
    }
}

// Called on the engine thread: invokes user callbacks for completed jobs.
void Http::drain ()
{
    while(this->done.size() > 0)
    {
        Job job = this->done.frontPop();
        if(job.complete) job.complete(job);
    }
}

// Application worker implementation for the HTTP plugin.
// Performs blocking network I/O on a dedicated thread and reports results via
// a thread-safe queue to be drained on the engine thread.
#include "Http.hpp"
#include <regex>
#include <openssl/x509v3.h>

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
        //httplib::Client
        //httplib::SSLClient cli(, 443);

        std::regex urlPattern(R"(^(\w+):\/\/([\w.-]+)(\/\S*)?$)");
        std::smatch matches;

        if(!std::regex_search(job.url, matches, urlPattern))
        {
            job.succeed = false;
            job.errorMessage = "Invalid URL format";
            this->done.push(job);
            continue;
        }

        httplib::Result res;

        if(matches[1].str() == "https")
        {
            httplib::SSLClient cli(matches[2].str(), 443);

            // Execute the appropriate HTTP method
            switch(job.method)
            {
                case Method::Get:
                    res = cli.Get(matches[3].str(), job.headers);
                    break;
                case Method::Post:
                    res = cli.Post(matches[3].str(), job.headers, job.body, "application/json");
                    break;
                case Method::Put:
                    res = cli.Put(matches[3].str(), job.headers, job.body, "application/json");
                    break;
                case Method::Patch:
                    res = cli.Patch(matches[3].str(), job.headers, job.body, "application/json");
                    break;
                case Method::Delete:
                    res = cli.Delete(matches[3].str(), job.headers);
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
        }
        else if(matches[1].str() == "http")
        {
            httplib::Client cli(matches[2].str(), 80);

            // Execute the appropriate HTTP method
            switch(job.method)
            {
                case Method::Get:
                    res = cli.Get(matches[3].str(), job.headers);
                    break;
                case Method::Post:
                    res = cli.Post(matches[3].str(), job.headers, job.body, "application/json");
                    break;
                case Method::Put:
                    res = cli.Put(matches[3].str(), job.headers, job.body, "application/json");
                    break;
                case Method::Patch:
                    res = cli.Patch(matches[3].str(), job.headers, job.body, "application/json");
                    break;
                case Method::Delete:
                    res = cli.Delete(matches[3].str(), job.headers);
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

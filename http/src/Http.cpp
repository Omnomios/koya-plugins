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
            this->done.push(job);
            continue;
        }

        if(matches[1].str() == "https")
        {
            httplib::SSLClient cli(matches[2].str(), 443);

            httplib::Result res;

            if((res = cli.Get(matches[3].str())))
            {
                job.succeed = true;
                job.headers = res->headers;
                job.status = res->status;
                job.body = res->body;
            }
            else
            {
                job.succeed = false;
                std::cout << "error code: " << res.error() << std::endl;
                auto result = cli.get_openssl_verify_result();
                if (result)
                {
                    std::cout << "verify error: " << X509_verify_cert_error_string(result) << std::endl;
                }
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

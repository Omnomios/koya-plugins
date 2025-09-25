/*
   Koya Plugin Helper: Http worker

   What this is:
   - Application-side helper class used by the HTTP plugin to perform HTTP(S)
     requests on a background thread and hand results back to the main thread.

   Koya vs Application responsibilities:
   - Koya provides the engine loop and the update hook where we drain results.
   - This helper (application) owns the worker thread and queues.

   Usage inside the plugin:
   - `Http::request(method, url, callback)` enqueues a job. The `drain()` method
     should be called from the plugin's `update` hook to invoke user callbacks
     on the engine thread.

   This header intentionally avoids Koya-specific headers to keep separation of
   concerns. The integration with QuickJS and hooks happens in `module.cpp`.
*/
#pragma once

#include <thread>
#include <semaphore>
#include "httplib.h"
#include "../../sdk/threaded/Queue.hpp"

class Http
{
public:
    enum class Method
    {
        Get,
        Post,
        Patch,
        Put,
        Delete
    };

    struct Job
    {
        Method method = Method::Get;
        std::string url;
        bool succeed = false;
        bool binary = false;

        std::function<void(Job)> complete;

        httplib::Headers headers;
        std::string body;
        std::vector<uint8_t> bodyBinary;
        int status = 0;
    };

public:
    Http ();
    ~Http ();

    void request (const Method& method, const std::string& url, std::function<void(Job)> callback, const httplib::Headers& headers = {}, const std::string& body = "", bool binary = false)
    {
        Job job;
        job.method = method;
        job.url = url;
        job.complete = callback;
        job.headers = headers;
        job.body = body;
        job.binary = binary;
        this->waiting.push(job);
        this->jobSignal.release();
    }

    void drain ();

private:

    bool running = false;
    void worker ();

    std::binary_semaphore jobSignal;
    threaded::queue<Job> waiting;
    threaded::queue<Job> done;
    std::thread workThread;
};

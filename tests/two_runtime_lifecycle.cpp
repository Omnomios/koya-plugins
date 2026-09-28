#include <chrono>
#include <string>
#include <thread>

#include "service/NativeModuleHost.hpp"
#include "service/Scripting.hpp"

namespace
{
    bool start (Helix::Scripting& scripting, const std::string& searchPath, const std::string& module)
    {
        return scripting.startScriptThread([&] (Helix::Scripting& owner) {
            owner.nativeModules().setSearchPaths({searchPath});
            if(owner.nativeModules().load(owner.getContextPtr(), module.c_str()) == nullptr) return false;
            if(module == "Module/http")
            {
                try
                {
                    owner.getContext().eval(
                        "import { request } from 'Module/http';"
                        "request({url: 'invalid'}).catch(() => {});",
                        "<http-inline-completion-regression>",
                        JS_EVAL_TYPE_MODULE);
                }
                catch(const qjs::exception&)
                {
                    Helix::Scripting::printJSException(owner.getContextPtr());
                    return false;
                }
            }
            return true;
        });
    }

    void stop (Helix::Scripting& scripting)
    {
        scripting.stopScriptThread();
        scripting.nativeModules().shutdownEngine();
        scripting.nativeModules().destroy();
    }
}

int main (int argc, char** argv)
{
    if(argc != 3) return 2;
    const std::string module = std::string("Module/") + argv[2];
    Helix::Scripting first;
    Helix::Scripting second;
    if(!start(first, argv[1], module) || !start(second, argv[1], module)) return 3;

    first.nativeModules().scriptUpdate({1.0, 0.01});
    second.nativeModules().scriptUpdate({2.0, 0.02});
    first.nativeModules().engineSolve();
    second.nativeModules().engineRender();
    first.nativeModules().renderBegin({1, 1.0, 0.0});
    second.nativeModules().renderBegin({2, 2.0, 0.5});
    for(int attempt = 0; attempt < (argv[2] == std::string("http") ? 50 : 1); ++attempt)
    {
        first.nativeModules().scriptUpdate({1.0 + attempt, 0.01});
        second.nativeModules().scriptUpdate({2.0 + attempt, 0.02});
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    stop(first);
    second.nativeModules().scriptUpdate({3.0, 0.03});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stop(second);
    return 0;
}

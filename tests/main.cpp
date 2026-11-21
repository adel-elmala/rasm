#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#ifdef _WIN32
#include <crtdbg.h>
#include <cstdlib>
#include <windows.h>
#endif

namespace
{
    // A CRT assert or abort opens a dialog box by default, which waits forever on a machine nobody watches (CI).
    void reportErrorsToStderr()
    {
#ifdef _WIN32
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    }
}

int main(int argc, char **argv)
{
    reportErrorsToStderr();
    doctest::Context context(argc, argv);
    return context.run();
}

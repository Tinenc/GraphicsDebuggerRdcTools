// Trivial exe used as a sanity-test target for inject.ps1. Just prints a
// line so we can see "Image loaded" vs "Image rejected by loader".
#include <windows.h>
#include <stdio.h>
int main(int argc, char *argv[])
{
    printf("hello.exe pid=%lu argc=%d\n", GetCurrentProcessId(), argc);
    return 0;
}

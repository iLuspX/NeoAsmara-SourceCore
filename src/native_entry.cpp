#include <windows.h>
#ifndef ROSH_ROLE_ID
#error ROSH_ROLE_ID must identify the native service
#endif
extern "C" __declspec(dllimport) int RoshServerApplication(HINSTANCE, int, const wchar_t *, const wchar_t *);
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t *command, int show) {
    static constexpr const wchar_t* roles[]={L"LoginServer",L"MainServer",L"LobbyServer",L"WorldServer"};
    static_assert(ROSH_ROLE_ID>=0&&ROSH_ROLE_ID<4,"Invalid native service role");
    return RoshServerApplication(instance, show, roles[ROSH_ROLE_ID], command);
}

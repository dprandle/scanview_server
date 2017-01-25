#include <edcomm_system.h>
#include <edutility.h>
#include <edtimer.h>

#include <sys/types.h>
#include <unistd.h>

edtimer tmr;

int32_t main(int32_t argc, char * argv[])
{
    int pid = getpid();
    log_message("Starting scanview_server with pid " + std::to_string(pid) + " at " + timestamp(),"server_status.log", false);
    int32_t port = 0;
    int32_t ctrl_port = 0;
	for (int32_t i = 0; i < argc; ++i)
	{
		std::string curarg(argv[i]);
		if (curarg.find("-port:") == 0)
			port = std::stoi(curarg.substr(6));
        if (curarg.find("-ctrlmod_port:") == 0)
            ctrl_port = std::stoi(curarg.substr(14));
    }

    if (port == 0)
        port = 13367;
    if (ctrl_port == 0)
        ctrl_port = 13366;

    std::string command("fuser -k " + std::to_string(port) + "/tcp");
    system(command.c_str());

    tmr.start();
    tmr.update();

    scanview_server sver;
    sver.running = true;
    sver.set_port(static_cast<uint16_t>(port));
    sver.init();

    sver._restart_ctrlmod(static_cast<int16_t>(ctrl_port));

    while(sver.running)
    {
        sver.update();
        tmr.update();
        cprint_flush();
    }
    tmr.stop();
    log_message("Ended server execution after " + std::to_string(tmr.elapsed()) + " on " + timestamp());
    sver.release();

    return 0;
}

#ifndef EDCOMM_SYSTEM_H
#define EDCOMM_SYSTEM_H

#include <vector>
#include <edutility.h>

#define SOCKET_BUFF_SIZE
#define HEADER_BYTE_SIZE 8

#define RESTART_CTRLMOD "restart_ctrlmod"
#define KILL_CTRLMOD "kill_ctrlmod"
#define REBOOT_EDISON "reboot_edison"
#define UPDATE_FIRMWARE "update_firmware"
#define GET_LOG_FILES "get_log_files"
#define CLEAR_LOGS "clear_logs"
#define COMMAND_ACK "command_ack"
#define SETUP_EDISON_STARTUP "setup_edison_startup"
#define DATA_ACK "data_ack"
#define SERVER_CONSOLE_LOG "server_console.log"
#define SERVER_STATUS_LOG "server_status.log"
#define CONSOLE_LOG "ctrlmod_console.log"
#define STATUS_LOG "ctrlmod_status.log"
#define GET_FIRMWARE "get_firmware"

#define DEFAULT_PROG_DIR "/home/root/progs/"
#define DEFAULT_CTRLMOD_NAME "ctrlmod"


class edsocket;

struct server_packet_header
{
    server_packet_header();
    void clear();
    union
    {
        struct
        {
            uint32_t hash_id;
            uint32_t data_size;
        };
        uint8_t data[HEADER_BYTE_SIZE];
    };
};

enum current_state
{
    cs_idle,
    cs_receiving_header,
    cs_receiving_data
};

struct firmware_ret
{
    uint32_t old_fw_size;
    uint32_t new_fw_size;
};

class scanview_server
{
  public:

    scanview_server();

    ~scanview_server();

    void init();

    void release();

    void write_version_file();

    bool read_version_file();

	uint16_t port();

	void set_port(uint16_t port_);

    uint16_t ctrlmod_port();

    void set_ctrlmod_port(uint16_t port);

    void update();

    uint32_t recvFromClients(uint8_t * data, uint32_t max_size);

	void sendToClients(uint8_t * data, uint32_t size);

    bool running;

    void _kill_ctrlmod();
    void _restart_ctrlmod(int16_t port);
    void _reboot_edison();
    void _clear_logs();

  private:

	typedef std::vector<edsocket*> ClientArray;
	
    void _handle_byte(uint8_t byte);

    void _received_header();
    void _received_all_data();

	void _clean_closed_connections();

    void _update_firmware(firmware_ret * fr);
    void _get_log_files();

    void _send_ack(const std::string & str);
    void _send_data_ack(uint32_t bytes);
    void _send_log_file(const std::string & which, const std::string & path);
    void _setup_edison_startup();

	ClientArray m_clients;
	
	int32_t m_server_fd;
	uint16_t m_port;
    uint16_t m_ctrlmod_port;
    uint32_t m_cur_index;
    pid_t m_cur_child;

    current_state m_cur_state;
    server_packet_header m_cur_header;
    std::vector<uint8_t> m_cur_data;

    std::string m_prog_dir;
    std::string m_prog_name;

    uint32_t m_data_ack_ind;
};


#endif

#include <edcomm_system.h>
#include <edsocket.h>
#include <edutility.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>
#include <sstream>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

server_packet_header::server_packet_header()
{
    clear();
}

void server_packet_header::clear()
{
    zero_buf(data,HEADER_BYTE_SIZE);
}

scanview_server::scanview_server():
    running(false),
    m_server_fd(0),
    m_port(0),
    m_ctrlmod_port(0),
    m_cur_index(0),
    m_cur_child(0),
    m_cur_state(cs_idle),
    m_cur_header(),
    m_cur_data(),
    m_prog_dir(DEFAULT_PROG_DIR),
    m_prog_name(DEFAULT_CTRLMOD_NAME),
    m_data_ack_ind(0)
{}

scanview_server::~scanview_server()
{}

uint16_t scanview_server::ctrlmod_port()
{
    return m_ctrlmod_port;
}

void scanview_server::set_ctrlmod_port(uint16_t port)
{
    m_ctrlmod_port = port;
}
	
void scanview_server::init()
{
	sockaddr_in server;
    m_server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (m_server_fd < 0)
    {
        int err = errno;
        log_message("edcomm_system::init Could not bind server - error: " + std::string(strerror(err)));
    }

    int optval = 1;
    setsockopt(m_server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));
    server.sin_addr.s_addr = INADDR_ANY;
	server.sin_family = AF_INET;
    server.sin_port = htons(m_port);

    if (bind(m_server_fd, (struct sockaddr *) &server, sizeof(server)) < 0)
    {
        int err = errno;
        log_message("edcomm_system::init Could not bind server - error: " + std::string(strerror(err)));
    }
	listen(m_server_fd, 5);
    log_message("edcomm_system::init Listening on port " + std::to_string(m_port));

    if (!read_version_file())
        write_version_file();
}

uint16_t scanview_server::port()
{
	return m_port;
}

void scanview_server::set_port(uint16_t port_)
{
	m_port = port_;
}

void scanview_server::release()
{
    while (m_clients.begin() != m_clients.end())
    {
        delete m_clients.back();
        m_clients.pop_back();
    }
    close(m_server_fd);
}

uint32_t scanview_server::recvFromClients(uint8_t * data, uint32_t max_size)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < m_clients.size(); ++i)
    {
        uint32_t ret = m_clients[i]->read(data+total, max_size-total);
        if (ret != static_cast<uint32_t>(-1))
            total += ret;
    }
    return total;
}

void scanview_server::sendToClients(uint8_t * data, uint32_t size)
{
    for (uint32_t i = 0; i < m_clients.size(); ++i)
        m_clients[i]->write(data, size);
}

void scanview_server::update()
{
    sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int32_t sockfd = accept(m_server_fd, (struct sockaddr *) &client_addr, &client_len);
    if (sockfd != -1)
    {
        edsocket * new_client = new edsocket(sockfd);
        if (!new_client->start())
        {
            log_message("edcomm_system::update Received connection but could not start socket - should see deletion next");
        }
        m_clients.push_back(new_client);
        log_message("edcomm_system::update Server recieved connection from " + std::string(inet_ntoa(client_addr.sin_addr)) + ":" + std::to_string(ntohs(client_addr.sin_port)));

    }

    // Check for closed connections and remove them if there are any
    _clean_closed_connections();

    static uint8_t buf[256];
    uint32_t cnt = recvFromClients(buf, 256);
    for (uint32_t i = 0; i < cnt; ++i)
        _handle_byte(buf[i]);
}

void scanview_server::_clean_closed_connections()
{
    ClientArray::iterator iter = m_clients.begin();
    while (iter != m_clients.end())
    {
        if (!(*iter)->running())
        {
            sockaddr_in cl_addr;
            socklen_t cl_len = sizeof(cl_addr);
            getpeername((*iter)->fd(), (sockaddr *)&cl_addr, &cl_len);
            std::string client_ip = std::string(inet_ntoa(cl_addr.sin_addr)) + ":" + std::to_string(ntohs(cl_addr.sin_port));

            edthreaded_fd::Error er = (*iter)->error();
            std::string errno_message = strerror(er._errno);

            switch(er.err_val)
            {
              case(edthreaded_fd::ConnectionClosed):
                  log_message("Connection closed with " + client_ip);
                  break;
              case (edthreaded_fd::DataOverwrite):
                  log_message("Socket internal buffer overwritten with new data before previous data was sent" + client_ip + "\nError: " + errno_message);
                  break;
              case (edthreaded_fd::InvalidRead):
                  log_message("Socket invalid read from " + client_ip + "\nError: " + errno_message);
                  break;
              case (edthreaded_fd::InvalidWrite):
                  log_message("Socket invalid write to " + client_ip + "\nError: " + errno_message);
                  break;
              case (edthreaded_fd::ThreadCreation):
                  log_message("Error in thread creation for connection with " + client_ip);
                  break;
              default:
                  log_message("No internal error but socket thread not running with " + client_ip);
                  break;
            }
            delete (*iter);
            iter = m_clients.erase(iter);
        }
        else
            ++iter;
    }
}

void scanview_server::_handle_byte(uint8_t byte)
{
    if (m_cur_state == cs_idle)
        m_cur_state = cs_receiving_header;

    if (m_cur_state == cs_receiving_header)
    {
        m_cur_header.data[m_cur_index] = byte;
        ++m_cur_index;
        if (m_cur_index == HEADER_BYTE_SIZE)
            _received_header();
    }
    else
    {
        m_cur_data[m_cur_index] = byte;
        ++m_cur_index;
        ++m_data_ack_ind;

        if (m_data_ack_ind >= 1024)
        {
            _send_data_ack(m_cur_index);
            m_data_ack_ind = 0;
        }

        if (m_cur_index == m_cur_data.size())
        {
            _received_all_data();
            m_data_ack_ind = 0;
        }
    }

}

void scanview_server::_received_header()
{
    if (m_cur_header.hash_id == hash_id(KILL_CTRLMOD))
    {
        log_message("Received header for kill_ctrlmod command");
        _kill_ctrlmod();
        _send_ack(KILL_CTRLMOD);
    }
    else if (m_cur_header.hash_id == hash_id(REBOOT_EDISON))
    {
        log_message("Received header for reboot command");
        _send_ack(std::string(REBOOT_EDISON));
        _reboot_edison();
    }
    else if (m_cur_header.hash_id == hash_id(GET_LOG_FILES))
    {
        log_message("Received header for get_log_files command");
        _get_log_files();
        _send_ack(GET_LOG_FILES);
    }
    else if (m_cur_header.hash_id == hash_id(GET_FIRMWARE))
    {
        log_message("Received header for get_firmware command");
        _send_ack(std::string(GET_FIRMWARE) + " " + m_prog_name);
    }
    else if (m_cur_header.hash_id == hash_id(CLEAR_LOGS))
    {
        log_message("Received header for clear_logs command");
        _clear_logs();
        _send_ack(CLEAR_LOGS);
    }
    else if (m_cur_header.hash_id == hash_id(SETUP_EDISON_STARTUP))
    {
        log_message("Received header for setup_edison_startup command");
        _setup_edison_startup();
        _send_ack(std::string(SETUP_EDISON_STARTUP) + " - about to reboot edison.... please wait about 45 seconds");
        _reboot_edison();
    }

    if (m_cur_header.data_size != 0)
    {
        m_cur_state = cs_receiving_data;
        m_cur_data.resize(m_cur_header.data_size);
    }
    else
    {
        m_cur_state = cs_idle;
        m_cur_header.clear();
    }
    m_cur_index = 0;
}

void scanview_server::_kill_ctrlmod()
{
    if (m_cur_child != 0)
    {
        int status;
        log_message("Attempting to kill ctrlmod with pid " + std::to_string(m_cur_child));
        kill(m_cur_child, SIGTERM);
        waitpid(m_cur_child,&status,0);
        m_cur_child = 0;
        log_message("Successfully killed ctrlmod");
    }
    else
    {
        log_message("Ctrlmod not running - no need to kill");
    }
}

void scanview_server::_restart_ctrlmod(int16_t port)
{
    _kill_ctrlmod();
    int pid = fork();
    if (pid == 0)
    {
        log_message("Starting ctrlmod... (process forked successfully)");

        // make sure the port is dead
        std::string command("fuser -k " + std::to_string(port) + "/tcp");
        system(command.c_str());

        log_message("Killed all processes on port " + std::to_string(port));

        std::string arg = "-port:" + std::to_string(port);
        int ret = 0;
        ret = execl((m_prog_dir + m_prog_name).c_str(), m_prog_name.c_str(), arg.c_str(), nullptr);
        if (ret == -1)
        {
            int err = errno;
            std::string error_msg(strerror(err));
            log_message("Error executing ctrlmod: " + error_msg);
        }
        log_message("Ctrlmod has closed...");
        exit(0);
    }
    else if (pid == -1)
    {
        log_message("Error with creating new process for ctrlmod");
    }
    else
    {
        m_cur_child = pid;
        m_ctrlmod_port = port;
        log_message("Scanviewd continuing...");
    }
}

void scanview_server::_reboot_edison()
{
    log_message("About to issue reboot command");
    system("reboot");
}

void scanview_server::_update_firmware(firmware_ret * fr)
{
    // Can't update unless not running
    _kill_ctrlmod();

    // Get the old firmware exe size
    int fd = open((m_prog_dir + m_prog_name).c_str(), O_RDONLY);
    if (fd != -1)
    {
        fr->old_fw_size = static_cast<uint32_t>(lseek(fd, 0L, SEEK_END));
        close(fd);
    }
    else
    {
        log_message("scanview_server::_update_firmware Could not open ctrlmod to get size - possibly missing file?");
    }

    system(("rm " + m_prog_dir + m_prog_name).c_str());

    uint8_t fwname_size = m_cur_data[0];
    m_prog_name = std::string(reinterpret_cast<char*>(&m_cur_data[1]), fwname_size);

    // New firware size should be same as data size
    fr->new_fw_size = m_cur_data.size() - fwname_size - 1;

    // Write out the new firmware to the exe
    fd = open((m_prog_dir + m_prog_name).c_str(), O_WRONLY | O_TRUNC | O_CREAT, S_IRWXU);
    if (fd != -1)
    {
        write(fd, m_cur_data.data()+1+fwname_size, fr->new_fw_size);
        log_message("Successfully updated firmware");
        close(fd);
        write_version_file();
    }
    else
    {
        std::string err_str(strerror(errno));
        cprint("Could not update firmware - unable to open file: " + err_str);
        read_version_file();
    }
}

void scanview_server::_get_log_files()
{
    log_message("Attempting to get log files...");
    _send_log_file(SERVER_CONSOLE_LOG,m_prog_dir + SERVER_CONSOLE_LOG);
    _send_log_file(SERVER_STATUS_LOG,m_prog_dir + SERVER_STATUS_LOG);
    _send_log_file(CONSOLE_LOG,m_prog_dir + CONSOLE_LOG);
    _send_log_file(STATUS_LOG,m_prog_dir + STATUS_LOG);
}

void scanview_server::_setup_edison_startup()
{
    system("mkdir /etc/init.d");
    int fd = open("/etc/init.d/scanview_server_startup.sh", O_WRONLY | O_TRUNC | O_CREAT, S_IRWXU);
    std::string script("#!/bin/sh\ncd " + m_prog_dir + "\n./scanview_server -port:" + std::to_string(m_port) + " -ctrlmod_port:" + std::to_string(m_ctrlmod_port));
    if (fd != -1)
    {
        log_message("Setting up startup script...\n" + script);
        write(fd, script.c_str(), script.size());
        close(fd);
        system("update-rc.d scanview_server_startup.sh defaults");
    }
    else
    {
        log_message("Could not open init script file");
    }
}

void scanview_server::_send_log_file(const std::string & which, const std::string & path)
{
    server_packet_header resp;
    resp.hash_id = hash_id(which);
    // Get the old firmware exe size
    int fd = open(path.c_str(), O_RDONLY);
    if (fd != -1)
    {
        uint32_t bytes = static_cast<uint32_t>(lseek(fd, 0L, SEEK_END));
        lseek(fd, 0L, SEEK_SET);
        uint8_t * data = new uint8_t[bytes];
        read(fd, data, bytes);
        close(fd);
        log_message("Sending " + path + " of " + std::to_string(bytes) + " bytes to clients");
        resp.data_size = bytes;
        sendToClients(resp.data, HEADER_BYTE_SIZE);
        sendToClients(data, bytes);
        delete [] data;
    }
    else
    {
        log_message("Problem in sending log file - could not open " + path);
    }
}

void scanview_server::_clear_logs()
{
    log_message("Clearing log files...");
    system("rm /home/root/*.log");
    system("rm /home/root/progs/*.log");
}

bool scanview_server::read_version_file()
{
    int infd = open((m_prog_dir + "version").c_str(), O_RDONLY);
    if (infd != -1)
    {
        int fsz = lseek(infd, 0L, SEEK_END);

        if (fsz > 0)
        {
            char * data = new char[fsz];
            lseek(infd, 0L, SEEK_SET);
            read(infd, data, fsz);
            m_prog_name = std::string(data);
            m_prog_name.resize(fsz);
            log_message("Current firmware version read as " + m_prog_name);
            delete [] data;
        }
        else
        {
            log_message("Version file seek returned bad value for some reason");
        }
        close(infd);
        return true;
    }
    else
    {
        log_message("Version file does not exist - " + std::string(strerror(errno)));
        return false;
    }
}

void scanview_server::write_version_file()
{
    int infd = open((m_prog_dir + "version").c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (infd != -1)
    {
        log_message("Writing version " + m_prog_name + " to version file");
        write(infd, m_prog_name.c_str(), m_prog_name.size());
        close(infd);
    }
    else
    {
        log_message("Failed to open version file for writing... " + std::string(strerror(errno)));
    }
}

void scanview_server::_send_ack(const std::string & str)
{
    log_message("Sending acknowledge packet: " + str);
    server_packet_header resp;
    resp.hash_id = hash_id(COMMAND_ACK);
    resp.data_size = str.size();
    sendToClients(resp.data, HEADER_BYTE_SIZE);
    sendToClients((uint8_t*)str.data(), str.size());
}

void scanview_server::_send_data_ack(uint32_t bytes)
{
    server_packet_header resp;
    resp.hash_id = hash_id(DATA_ACK);
    resp.data_size = bytes;
    sendToClients(resp.data, HEADER_BYTE_SIZE);
}

void scanview_server::_received_all_data()
{
    server_packet_header resp;
    resp.hash_id = hash_id(COMMAND_ACK);

    if (m_cur_header.hash_id == hash_id(UPDATE_FIRMWARE))
    {
        std::string prev_ver = m_prog_name;
        firmware_ret fr = {};
        _update_firmware(&fr);
        log_message("Received ctrlmod firmware update command");
        // Compose ack message
        _send_ack("Successfully updated - old firmware " + prev_ver + " (" + std::to_string(double(fr.old_fw_size) / 1024.0) + " KB) replaced with new firmware " + m_prog_name + " (" + std::to_string(double(fr.new_fw_size) / 1024.0) + " KB)");
    }

    if (m_cur_header.hash_id == hash_id(RESTART_CTRLMOD))
    {
        int16_t port = int16_t(m_cur_data[0]) | (int16_t(m_cur_data[1]) << 8);
        log_message("Received ctrlmod restart command");
        _restart_ctrlmod(port);
        _send_ack(RESTART_CTRLMOD);
    }

    m_cur_index = 0;
    m_cur_state = cs_idle;
    m_cur_header.clear();
    m_cur_data.clear();
}

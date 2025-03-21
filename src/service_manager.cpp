#include <multibotnet/service_manager.hpp>
#include <multibotnet/ros_sub_pub.hpp>
#include <yaml-cpp/yaml.h>
#include <thread>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <zmq.hpp>

namespace multibotnet {

ServiceManager::ServiceManager() : context_(1) {}

ServiceManager::~ServiceManager() {
    for (auto& th : service_threads_) {
        if (th.joinable()) th.join();
    }
}

void ServiceManager::init(const std::string& config_file) {
    YAML::Node config = YAML::LoadFile(config_file);
    displayConfig(config);

    if (config["services"]) {
        // 预先为向量分配空间，避免重新分配导致引用失效
        size_t num_services = config["services"].size();
        rep_sockets_.reserve(num_services);
        service_servers_.reserve(num_services);
        service_threads_.reserve(num_services);

        for (const auto& service : config["services"]) {
            std::string service_name = service["service_name"].as<std::string>();
            std::string service_type = service["service_type"].as<std::string>();
            std::string src_ip = service["srcIP"].as<std::string>();
            int src_port = service["srcPort"].as<int>();

            if (src_ip == "self") {
                src_ip = getLocalIP();
            }

            startService(service_name, service_type, src_ip, src_port);
        }
    }
}

std::string ServiceManager::getLocalIP() {
    struct ifaddrs *ifaddr;
    std::string local_ip = "127.0.0.1";

    if (getifaddrs(&ifaddr) == -1) {
        ROS_ERROR("Failed to get local IP address: %s", strerror(errno));
        return local_ip;
    }

    for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
            char *ip = inet_ntoa(addr->sin_addr);
            if (strcmp(ip, "127.0.0.1") != 0) {
                local_ip = ip;
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
    if (local_ip == "127.0.0.1") {
        ROS_WARN("No non-loopback IP found, using 127.0.0.1");
    }
    return local_ip;
}

void ServiceManager::startService(const std::string& service_name, const std::string& service_type,
                                  const std::string& src_ip, int src_port) {
    zmq::socket_t rep_socket(context_, ZMQ_REP);
    std::string address = "tcp://" + src_ip + ":" + std::to_string(src_port);

    try {
        rep_socket.bind(address);
    } catch (const zmq::error_t& e) {
        ROS_ERROR("Failed to bind to %s: %s", address.c_str(), e.what());
        ROS_ERROR("Please check if the IP address is valid and the port %d is not in use.", src_port);
        return;
    }

    rep_sockets_.emplace_back(std::move(rep_socket));
    auto& current_socket = rep_sockets_.back();

    ros::NodeHandle nh;
    ros::ServiceServer server;

    if (service_type == "std_srvs/SetBool") {
        server = nh.advertiseService(service_name, &ServiceManager::handleSetBool, this);
    } else if (service_type == "nav_msgs/GetPlan") {
        server = nh.advertiseService(service_name, &ServiceManager::handleGetPlan, this);
    } else {
        ROS_ERROR("Unsupported service type: %s", service_type.c_str());
        return;
    }
    service_servers_.push_back(server);

    service_threads_.emplace_back([this, &current_socket, service_name, service_type]() {
        while (ros::ok()) {
            zmq::pollitem_t items[] = {{static_cast<void*>(current_socket), 0, ZMQ_POLLIN, 0}};
            zmq::poll(items, 1, 100); // 100ms timeout

            if (!ros::ok()) break;

            if (items[0].revents & ZMQ_POLLIN) {
                zmq::message_t request;
                if (current_socket.recv(request)) {
                    if (service_type == "std_srvs/SetBool") {
                        auto req = deserializeMsg<std_srvs::SetBool::Request>(
                            static_cast<uint8_t*>(request.data()), request.size());
                        std_srvs::SetBool::Response res;
                        handleSetBool(req, res);
                        auto buffer = serializeMsg(res);
                        zmq::message_t reply(buffer.size());
                        memcpy(reply.data(), buffer.data(), buffer.size());
                        current_socket.send(reply);
                    } else if (service_type == "nav_msgs/GetPlan") {
                        auto req = deserializeMsg<nav_msgs::GetPlan::Request>(
                            static_cast<uint8_t*>(request.data()), request.size());
                        nav_msgs::GetPlan::Response res;
                        handleGetPlan(req, res);
                        auto buffer = serializeMsg(res);
                        zmq::message_t reply(buffer.size());
                        memcpy(reply.data(), buffer.data(), buffer.size());
                        current_socket.send(reply);
                    }
                }
            }
        }
    });
}

bool ServiceManager::handleSetBool(std_srvs::SetBool::Request& req, std_srvs::SetBool::Response& res) {
    res.success = true;
    res.message = "Service called with data: " + std::to_string(req.data);
    return true;
}

bool ServiceManager::handleGetPlan(nav_msgs::GetPlan::Request& req, nav_msgs::GetPlan::Response& res) {
    res.plan.header.stamp = ros::Time::now();
    res.plan.header.frame_id = "map";
    return true;
}

void ServiceManager::displayConfig(const YAML::Node& config) {
    ROS_INFO("-------services-------");
    if (config["services"]) {
        for (const auto& service : config["services"]) {
            std::string service_name = service["service_name"].as<std::string>();
            std::string src_ip = service["srcIP"].as<std::string>();
            ROS_INFO("%s  (at %s)", service_name.c_str(), src_ip.c_str());
        }
    }
}

} // namespace multibotnet
#include "controller.hpp"

#include <memory>

#include <unitree/robot/channel/channel_factory.hpp>
#include <unitree/robot/g1/loco/g1_loco_client.hpp>

namespace mea {
namespace {

class G1Backend final : public Backend {
public:
    G1Backend() {
        client_.SetTimeout(0.1f);
        client_.Init();
    }

    int send(const Command& command) override {
        const auto& velocity = command.velocity;
        return client_.Move(static_cast<float>(velocity.vx), static_cast<float>(velocity.vy),
                            static_cast<float>(velocity.vyaw), false);
    }

private:
    unitree::robot::g1::LocoClient client_;
};

}  // namespace

std::unique_ptr<Backend> make_g1_backend(const std::string& interface) {
    unitree::robot::ChannelFactory::Instance()->Init(0, interface);
    return std::make_unique<G1Backend>();
}

}  // namespace mea

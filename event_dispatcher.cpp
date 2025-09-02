#include "event_dispatcher.h"
#include "dispatcher_select.h"
#include "dispatcher_epoll.h"

std::unique_ptr<Eventloop> EventLoopFactory::create_event_loop(EventType type) {
    switch (type) {
        case EventType::Select:
            return std::make_unique<dispatcherselect>();
            break;
        case EventType::Poll:
            // return std::make_unique<dispatcherpoll>();
            break;
        case EventType::Epoll:
            return std::make_unique<dispatcherepoll>();
            break;
        case EventType::AUTO:
            // Default to Select for AUTO
            return std::make_unique<dispatcherselect>();
            break;
        default:
            throw std::invalid_argument("Unknown event type");
    }
}


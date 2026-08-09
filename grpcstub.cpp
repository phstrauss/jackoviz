/*
 * grpcstub.cpp — gRPC server that forwards RPCs to jackoviz set_*() helpers.
 *
 * Generate stubs from jvzcontroller.proto, e.g.:
 *   protoc -I. --cpp_out=. --grpc_out=. \
 *     --plugin=protoc-gen-grpc=`which grpc_cpp_plugin` \
 *     jvzcontroller.proto
 *
 * Then link this translation unit with jackoviz.o and the generated
 * jvzcontroller.pb.cc / jvzcontroller.grpc.pb.cc plus libgrpc++.
 */

#include "jackoviz.h"
#include "jvzcontroller.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <cstdio>
#include <memory>
#include <string>
#include <thread>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

namespace {

ViewMode to_c_view_mode(jvz::ViewMode mode)
{
    switch (mode)
    {
    case jvz::VIEW_MODE_2D:
        return VIEW_MODE_2D;
    case jvz::VIEW_MODE_1D:
        return VIEW_MODE_1D;
    case jvz::VIEW_MODE_SCOPE:
        return VIEW_MODE_SCOPE;
    case jvz::VIEW_MODE_3D:
    default:
        return VIEW_MODE_3D;
    }
}

class JvzControllerServiceImpl final : public jvz::JvzController::Service
{
public:
    explicit JvzControllerServiceImpl(Jackoviz* app) : app_(app) {}

    Status SetDbFloor(ServerContext*, const jvz::SetDbFloorRequest* request,
        jvz::SetResponse* response) override
    {
        if (app_ == nullptr)
        {
            response->set_ok(false);
            response->set_message("no jackoviz instance");
            return Status::OK;
        }
        set_db_floor(app_, request->floor_db());
        response->set_ok(true);
        return Status::OK;
    }

    Status SetDbCeil(ServerContext*, const jvz::SetDbCeilRequest* request,
        jvz::SetResponse* response) override
    {
        if (app_ == nullptr)
        {
            response->set_ok(false);
            response->set_message("no jackoviz instance");
            return Status::OK;
        }
        set_db_ceil(app_, request->ceil_db());
        response->set_ok(true);
        return Status::OK;
    }

    Status SetViewMode(ServerContext*, const jvz::SetViewModeRequest* request,
        jvz::SetResponse* response) override
    {
        if (app_ == nullptr)
        {
            response->set_ok(false);
            response->set_message("no jackoviz instance");
            return Status::OK;
        }
        set_view_mode(app_, to_c_view_mode(request->mode()));
        response->set_ok(true);
        return Status::OK;
    }

    Status SetLineWidth(ServerContext*, const jvz::SetLineWidthRequest* request,
        jvz::SetResponse* response) override
    {
        if (app_ == nullptr)
        {
            response->set_ok(false);
            response->set_message("no jackoviz instance");
            return Status::OK;
        }
        set_line_width(app_, request->width_px());
        response->set_ok(true);
        return Status::OK;
    }

    Status SetPause(ServerContext*, const jvz::SetPauseRequest* request,
        jvz::SetResponse* response) override
    {
        if (app_ == nullptr)
        {
            response->set_ok(false);
            response->set_message("no jackoviz instance");
            return Status::OK;
        }
        set_pause(app_, request->paused());
        response->set_ok(true);
        return Status::OK;
    }

    Status SetPlotFreq(ServerContext*, const jvz::SetPlotFreqRequest* request,
        jvz::SetResponse* response) override
    {
        if (app_ == nullptr)
        {
            response->set_ok(false);
            response->set_message("no jackoviz instance");
            return Status::OK;
        }
        set_plot_freq(app_, request->freq_hz());
        response->set_ok(true);
        return Status::OK;
    }

    Status SetKaiserBeta(ServerContext*, const jvz::SetKaiserBetaRequest* request,
        jvz::SetResponse* response) override
    {
        if (app_ == nullptr)
        {
            response->set_ok(false);
            response->set_message("no jackoviz instance");
            return Status::OK;
        }
        set_kaiser_beta(app_, request->beta());
        response->set_ok(true);
        return Status::OK;
    }

private:
    Jackoviz* app_;
};

} // namespace

extern "C" void jvz_grpc_serve(Jackoviz* app, const char* listen_addr)
{
    const std::string address =
        (listen_addr != nullptr && listen_addr[0] != '\0') ? listen_addr : "0.0.0.0:50051";

    JvzControllerServiceImpl service(app);
    ServerBuilder builder;
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    if (!server)
    {
        std::fprintf(stderr, "jvz gRPC: failed to listen on %s\n", address.c_str());
        return;
    }
    std::fprintf(stderr, "jvz gRPC: listening on %s\n", address.c_str());
    server->Wait();
}

extern "C" void jvz_grpc_serve_async(Jackoviz* app, const char* listen_addr)
{
    const std::string address =
        (listen_addr != nullptr && listen_addr[0] != '\0') ? listen_addr : "0.0.0.0:50051";
    std::thread([app, address]() { jvz_grpc_serve(app, address.c_str()); }).detach();
}

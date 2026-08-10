/*
 * grpcstub.cpp — gRPC server that forwards RPCs to jackoviz set_*() helpers.
 *
 * Generated stubs come from jvzcontroller.proto via CMake (build/generated/).
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

void fill_double_response(jvz::SetResponse* response, double value, bool ok, const char* err_msg)
{
    response->set_ok(ok);
    response->set_value(value);
    response->set_code(0);
    if (!ok && err_msg != nullptr)
        response->set_message(err_msg);
}

void fill_int_response(jvz::SetResponse* response, int code, bool ok, const char* err_msg)
{
    response->set_ok(ok);
    response->set_code(code);
    response->set_value(0.0);
    if (!ok && err_msg != nullptr)
        response->set_message(err_msg);
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
            fill_double_response(response, 1.0, false, "no jackoviz instance");
            return Status::OK;
        }
        const double value = set_db_floor(app_, request->floor_db());
        /* C API uses 1.0 as the error sentinel (valid floors are negative). */
        fill_double_response(response, value, value != 1.0, "set_db_floor rejected");
        return Status::OK;
    }

    Status SetDbCeil(ServerContext*, const jvz::SetDbCeilRequest* request,
        jvz::SetResponse* response) override
    {
        if (app_ == nullptr)
        {
            fill_double_response(response, 1.0, false, "no jackoviz instance");
            return Status::OK;
        }
        const double value = set_db_ceil(app_, request->ceil_db());
        fill_double_response(response, value, value != 1.0, "set_db_ceil rejected");
        return Status::OK;
    }

    Status SetViewMode(ServerContext*, const jvz::SetViewModeRequest* request,
        jvz::SetResponse* response) override
    {
        if (app_ == nullptr)
        {
            fill_int_response(response, -1, false, "no jackoviz instance");
            return Status::OK;
        }
        const int code = set_view_mode(app_, to_c_view_mode(request->mode()));
        fill_int_response(response, code, code >= 0, "set_view_mode rejected");
        return Status::OK;
    }

    Status SetLineWidth(ServerContext*, const jvz::SetLineWidthRequest* request,
        jvz::SetResponse* response) override
    {
        if (app_ == nullptr)
        {
            fill_double_response(response, -1.0, false, "no jackoviz instance");
            return Status::OK;
        }
        const float value = set_line_width(app_, request->width_px());
        fill_double_response(response, (double)value, value >= 0.0f, "set_line_width rejected");
        return Status::OK;
    }

    Status SetPause(ServerContext*, const jvz::SetPauseRequest* request,
        jvz::SetResponse* response) override
    {
        if (app_ == nullptr)
        {
            fill_int_response(response, -1, false, "no jackoviz instance");
            return Status::OK;
        }
        const int code = set_pause(app_, request->paused());
        fill_int_response(response, code, code >= 0, "set_pause rejected");
        return Status::OK;
    }

    Status SetPlotFreq(ServerContext*, const jvz::SetPlotFreqRequest* request,
        jvz::SetResponse* response) override
    {
        if (app_ == nullptr)
        {
            fill_double_response(response, -1.0, false, "no jackoviz instance");
            return Status::OK;
        }
        const double value = set_plot_freq(app_, request->freq_hz());
        fill_double_response(response, value, value >= 0.0, "set_plot_freq rejected");
        return Status::OK;
    }

    Status SetKaiserBeta(ServerContext*, const jvz::SetKaiserBetaRequest* request,
        jvz::SetResponse* response) override
    {
        if (app_ == nullptr)
        {
            fill_double_response(response, -1.0, false, "no jackoviz instance");
            return Status::OK;
        }
        const double value = set_kaiser_beta(app_, request->beta());
        fill_double_response(response, value, value >= 0.0, "set_kaiser_beta rejected");
        return Status::OK;
    }

    Status Quit(ServerContext*, const jvz::QuitRequest*, jvz::SetResponse* response) override
    {
        if (app_ == nullptr)
        {
            fill_int_response(response, -1, false, "no jackoviz instance");
            return Status::OK;
        }
        const int code = quit(app_);
        fill_int_response(response, code, code >= 0, "quit rejected");
        return Status::OK;
    }

    Status ConnectJackPort(ServerContext*, const jvz::ConnectJackPortRequest* request,
        jvz::SetResponse* response) override
    {
        if (app_ == nullptr)
        {
            fill_int_response(response, -1, false, "no jackoviz instance");
            return Status::OK;
        }
        const int code = connect_jack_port(app_, request->port_name().c_str());
        fill_int_response(
            response, code, code >= 0,
            code >= 0 ? nullptr : "connect_jack_port failed");
        if (code >= 0)
            response->set_message(request->port_name());
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

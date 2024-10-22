#include <iostream>

#include "zeromq_endpoint.h"
#include "zhelpers.hpp"
#include "gabriel.pb.h"
#include "ai_detection.pb.h"

#include <modal_pipe_client.h>
#include <modal_pipe_server.h>
#include <modal_start_stop.h>

#include <functional>
#include <memory>
#include <vector>
#include <zmq.hpp>

#define SERVER_NAME "steeleagle-os-onboard-compute"
#define PIPE_NAME "onboard-compute"
#define PIPE_LOCATION (MODAL_PIPE_DEFAULT_BASE_DIR PIPE_NAME "/")

//-----------------------------------------------------------------------------

using namespace std;
using namespace std::placeholders;
using namespace gabriel;
using namespace steeleagle;

//-----------------------------------------------------------------------------

unique_ptr<ComputeEngine> engine;

//-----------------------------------------------------------------------------

void ComputeEngine::SendResult(const vector<ai_detection_t>& detections) {
    ComputeResult compute_result;
    for (const ai_detection_t& detection : detections) {
        AIDetection detection_proto;

        // Set protobuf fields
        detection_proto.set_timestamp_ns(detection.timestamp_ns);
        detection_proto.set_class_id(detection.class_id);
        detection_proto.set_frame_id(detection.frame_id);
        detection_proto.set_class_name(detection.class_name);
        detection_proto.set_cam(detection.cam);
        detection_proto.set_class_confidence(detection.class_confidence);
        detection_proto.set_detection_confidence(detection.detection_confidence);
        detection_proto.set_x_min(detection.x_min);
        detection_proto.set_y_min(detection.y_min);
        detection_proto.set_x_max(detection.x_max);
        detection_proto.set_y_max(detection.y_max);

        *(compute_result.add_compute_result()) = detection_proto;
    }

    // Send results to client
    string serialized_msg;
    compute_result.SerializeToString(&serialized_msg);
    zmq::message_t reply(serialized_msg.size());
    memcpy(reply.data(), serialized_msg.data(), serialized_msg.size());

    socket.send(reply);
}

//-----------------------------------------------------------------------------

ComputeEngine::ComputeEngine(const string& address, int channel) :
    context(1),
    socket(context, ZMQ_REP),
    channel(channel) {

    socket.bind(address);
}

//-----------------------------------------------------------------------------

void ComputeEngine::HandleRequest() {
    cout << "Waiting for request from client" << endl;
    // Wait for next request from client
    string client_msg = s_recv(socket);

    InputFrame frame;
    if (!frame.ParseFromString(client_msg)) {
        cerr << "Could not parse message from client" << endl;
    }

    if (frame.payload_type() != PayloadType::IMAGE) {
        cerr << "Incorrect payload type received from client" << endl;
    }

    if (frame.payloads_size() != 1) {
        cerr << "Expected one frame from the client" << endl;
    }

    const string& frame_bytes = frame.payloads(0);

    cout << "Received frame from client successfully"<< endl;

    pipe_server_write(channel, frame_bytes.c_str(), frame_bytes.length());

    cout << "Sent frame to voxl-tflite-server" << endl;
}

//-----------------------------------------------------------------------------

static void tflite_server_cb(int ch, char *data, int bytes, void *context) {
    cout << "Received results from voxl-tflite-server" << endl;
    vector<ai_detection_t> detections;
    for (int off = 0; off + (int)sizeof(ai_detection_t) <= bytes; off += sizeof(ai_detection_t)) {
        ai_detection_t *detection = reinterpret_cast<ai_detection_t *>(data + off);
        printf("Class confidence: %f\n", detection->class_confidence);
        detections.push_back(*detection);
    }
    engine->SendResult(detections);
}

//-----------------------------------------------------------------------------

int main(int argc, char *argv[]) {
    if (argc < 3) {
        cerr << "Expected at least two args\n";
        exit(-1);
    }

    string host(argv[1]);
    string port(argv[2]);

    ostringstream oss;
    oss << "tcp://*" << ":" << port;

    cout << "ip address is " << oss.str() << endl;

    if (kill_existing_process(SERVER_NAME, 2.0) < -2) return -1;

    // start signal handler so we can exit cleanly
    if (enable_signal_handler() == -1) {
        fprintf(stderr, "ERROR: failed to start signal handler\n");
        return -1;
    }

    // Open client pipe
    int flags = CLIENT_FLAG_EN_SIMPLE_HELPER;
    const string& input_pipe = "run/mpa/tflite_data";
    int ret = pipe_client_open(0, input_pipe.c_str(), SERVER_NAME, flags, 10 * sizeof(ai_detection_t));

    if (ret) {
        cerr << "Error opening channel" << endl;
        pipe_print_error(ret);
        if (ret == PIPE_ERROR_SERVER_NOT_AVAILABLE) {
            cerr << "Server not available" << endl;
        }
        return -1;
    }

    make_pid_file(SERVER_NAME);

    int ch = pipe_client_get_next_available_channel();
    engine = make_unique<ComputeEngine>(oss.str(), ch);
    pipe_client_set_simple_helper_cb(ch, tflite_server_cb, nullptr);

    if (create_pipe(ch)) {
        cerr << "Failed to create pipe" << endl;
        return -1;
    }

    while (main_running) {
        engine->HandleRequest();

        // Send reply back to client
        //s_send(responder, string("World"));
    }

    printf("Starting shutdown sequence\n");
    pipe_server_close_all();
    remove_pid_file(SERVER_NAME);
    printf("exiting cleanly\n");
    return 0;
}

//-----------------------------------------------------------------------------

static int create_pipe(int ch) {
    int flags = SERVER_FLAG_EN_CONTROL_PIPE;
    pipe_info_t info = {
        PIPE_NAME, PIPE_LOCATION, "bytes", SERVER_NAME, MODAL_PIPE_DEFAULT_PIPE_SIZE };

    if (!pipe_server_create(ch, info, flags)) {
        return -1;
    }
    return 0;
}

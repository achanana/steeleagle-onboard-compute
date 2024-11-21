#include <iostream>

#include "onboard_compute_engine.h"
#include "zhelpers.hpp"
#include "gabriel.pb.h"
#include "onboard_compute.pb.h"

#include <modal_pipe_client.h>
#include <modal_pipe_server.h>
#include <modal_start_stop.h>

#include <chrono>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <zmq.hpp>

#define PROCESS_NAME "steeleagle-os-onboard-compute"
#define PIPE_NAME "onboardcompute"
#define PIPE_LOCATION (MODAL_PIPE_DEFAULT_BASE_DIR PIPE_NAME "/")
#define TFLITE_PIPE_NAME "tflite_data"
#define TFLITE_PIPE_LOCATION (MODAL_PIPE_DEFAULT_BASE_DIR TFLITE_PIPE_NAME "/")

//-----------------------------------------------------------------------------

using namespace std;
using namespace steeleagle;

//-----------------------------------------------------------------------------

unique_ptr<ComputeEngine> engine;

//-----------------------------------------------------------------------------

void ComputeEngine::AccumulateResults(vector<ai_detection_t>&& new_detections) {
    // Check for delimiter frame
    bool send_results = new_detections.back().frame_id == -1;
    accumulated_results.insert(accumulated_results.end(),
                               make_move_iterator(new_detections.begin()),
                               make_move_iterator(new_detections.end()));
    if (send_results) {
        SendResult();
    }
}

//-----------------------------------------------------------------------------

void ComputeEngine::SendResult() {
    ComputeResult compute_result;
    cout << "Sending " << accumulated_results.size() - 1 << " results to client" << endl;
    for (const ai_detection_t& detection : accumulated_results) {
        // Skip delimiter frame
        if (detection.frame_id == -1)
            continue;
        cout << "Detection from frame " << detection.frame_id << endl;
        AIDetection detection_proto;

        string class_name(detection.class_name);
        string cam(detection.cam);
        cout << "Class name: " << class_name << "; cam: " << cam << endl;

        // Set protobuf fields
        detection_proto.set_timestamp_ns(detection.timestamp_ns);
        detection_proto.set_class_id(detection.class_id);
        detection_proto.set_frame_id(detection.frame_id);
        detection_proto.set_class_name(class_name);
        detection_proto.set_cam(cam);
        detection_proto.set_class_confidence(detection.class_confidence);
        detection_proto.set_detection_confidence(detection.detection_confidence);
        detection_proto.set_x_min(detection.x_min);
        detection_proto.set_y_min(detection.y_min);
        detection_proto.set_x_max(detection.x_max);
        detection_proto.set_y_max(detection.y_max);

        *(compute_result.add_compute_result()) = detection_proto;
    }
    accumulated_results.clear();

    // Send results to client
    cout << "Sending result(s) to client" << endl;
    string serialized_msg;
    compute_result.SerializeToString(&serialized_msg);
    s_send(socket, serialized_msg);

    // Ready for next client request
    {
        lock_guard<mutex> lock(mtx);
        ready = true;
    }
    cv.notify_one();
    // zmq::message_t reply(serialized_msg.size());
    //memcpy(reply.data(), serialized_msg.data(), serialized_msg.size());

    //socket.send(reply);
}

//-----------------------------------------------------------------------------

ComputeEngine::ComputeEngine(
    const string& address,
    int server_channel,
    int client_channel) :
    context(1),
    socket(context, ZMQ_REP),
    server_channel(server_channel),
    client_channel(client_channel) {

    cout << "Binding on address " << address << endl;
    socket.bind(address);
}

//-----------------------------------------------------------------------------

void ComputeEngine::HandleRequest() {
    cout << "\nWaiting for request from client" << endl;
    // Wait for next request from client

    zmq::pollitem_t poll_item;
    poll_item.socket = &socket;
    poll_item.events = ZMQ_POLLIN;

    zmq::poll(&poll_item, 1, -1);

    string client_msg;
    if (poll_item.revents & ZMQ_POLLIN) {
        client_msg = s_recv(socket);
    } else {
        cout << "Poller returned prematurely" << endl;
        return;
    }

    ComputeRequest request;
    if (!request.ParseFromString(client_msg)) {
        cerr << "Could not parse message from client" << endl;
    }

    const string& frame_bytes = request.frame_data();

    cout << "Received frame from client successfully"<< endl;

    camera_image_metadata_t cam_meta;
    cam_meta.magic_number = CAMERA_MAGIC_NUMBER;
    cam_meta.frame_id = ++frame_id;
    cam_meta.width = request.frame_width();
    cam_meta.height = request.frame_height();
    cam_meta.size_bytes = frame_bytes.size();
    cam_meta.format = IMAGE_FORMAT_YUV422;

    // pipe_server_write(server_channel, &cam_meta,
    //                   sizeof(camera_image_metadata_t));
    // pipe_server_write(server_channel, frame_bytes.data(), frame_bytes.size());
    if (pipe_server_write_camera_frame(server_channel, cam_meta, frame_bytes.data())) {
        cerr << "Error writing camera frame to server pipe" << endl;
    }

    cout << "Sent frame to voxl-tflite-server" << endl;

    // Send results back before waiting for next request from client
    unique_lock<mutex> lock(mtx);
    ready = false;

    while (!cv.wait_for(lock, chrono::seconds(1), [&] { return ready || !main_running; }));
}

//-----------------------------------------------------------------------------

static void tflite_server_cb(int ch, char *data, int bytes, void *context) {
    cout << "Received results from voxl-tflite-server" << endl;
    vector<ai_detection_t> detections;

    for (int off = 0; off + (int)sizeof(ai_detection_t) <= bytes;
            off += sizeof(ai_detection_t)) {
        ai_detection_t *detection =
            reinterpret_cast<ai_detection_t *>(data + off);
        printf("Class confidence: %f\n", detection->class_confidence);
        detections.push_back(*detection);
    }
    engine->AccumulateResults(move(detections));
}

//-----------------------------------------------------------------------------

int main(int argc, char *argv[]) {
    if (argc < 3) {
        cerr << "Expected at least two args\n";
        return -1;
    }

    string host(argv[1]);
    string port(argv[2]);

    ostringstream oss;
    oss << "tcp://*" << ":" << port;

    cout << "ip address is " << oss.str() << endl;

    if (kill_existing_process(PROCESS_NAME, 2.0) < -2) return -1;

    // start signal handler so we can exit cleanly
    if (enable_signal_handler() == -1) {
        fprintf(stderr, "ERROR: failed to start signal handler\n");
        return -1;
    }
    make_pid_file(PROCESS_NAME);

    int server_ch = pipe_client_get_next_available_channel();
    int client_ch = pipe_client_get_next_available_channel();

    engine = make_unique<ComputeEngine>(oss.str(), server_ch, client_ch);
    pipe_client_set_simple_helper_cb(client_ch, tflite_server_cb, nullptr);

    if (create_server_pipe(server_ch)) {
        cerr << "Failed to create server pipe" << endl;
        return -1;
    }

    if (create_client_pipe(client_ch)) {
        cerr << "Failed to create client pipe" << endl;
        return -1;
    }

    main_running = 1;

    while (main_running) {
        engine->HandleRequest();
    }

    printf("Starting shutdown sequence\n");
    pipe_client_flush(client_ch);
    pipe_server_close_all();
    remove_pid_file(PROCESS_NAME);
    printf("exiting cleanly\n");
    return 0;
}

//-----------------------------------------------------------------------------

static int create_server_pipe(int ch) {
    pipe_info_t info = {
        PIPE_NAME, PIPE_LOCATION, "camera", PROCESS_NAME,
        16 * MODAL_PIPE_DEFAULT_PIPE_SIZE, 0
    };

    if (pipe_server_create(ch, info, 0)) {
        return -1;
    }
    return 0;
}

//-----------------------------------------------------------------------------

static int create_client_pipe(int ch) {
    int ret = pipe_client_open(ch, TFLITE_PIPE_LOCATION, PROCESS_NAME,
                               CLIENT_FLAG_EN_SIMPLE_HELPER,
                               10 * sizeof(ai_detection_t));
    if (ret) {
        pipe_print_error(ret);
    }
    return ret;
}

//-----------------------------------------------------------------------------

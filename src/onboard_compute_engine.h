#include <ai_detection.h>

#include <condition_variable>
#include <mutex>
#include <string>

#include "zmq.hpp"

using namespace std;

static int create_server_pipe(int ch);
static int create_client_pipe(int ch);

class ComputeEngine {
 public:
    ComputeEngine(const string& address, int server_channel, int client_channel);
    void HandleRequest();
    void TfliteServerCb(int ch, char *data, int bytes, void *context);
    void SendResult();
    void AccumulateResults(vector<ai_detection_t>&& new_detections);

 private:
    int frame_id = 0;
    zmq::context_t context;
    zmq::socket_t socket;
    int server_channel;
    int client_channel;
    mutex mtx;
    condition_variable cv;
    bool ready;
    vector<ai_detection_t> accumulated_results;
};

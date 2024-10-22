#include <ai_detection.h>

#include <string>

#include "zmq.hpp"

using namespace std;

static int create_pipe(int ch);

class ComputeEngine {
 public:
    ComputeEngine(const string& address, int channel);
    void HandleRequest();
    void TfliteServerCb(int ch, char *data, int bytes, void *context);
    void SendResult(const vector<ai_detection_t>& detections);

 private:
    zmq::context_t context;
    zmq::socket_t socket;
    int channel;
};

#ifndef OV2640_H_
#define OV2640_H_

#include "esp_camera.h"

#define DETECTION_SWITCH 0
#define RECOGNITION_SWITCH 0

class OV2640
{
public:
    OV2640(){
        fb = NULL;
    };
    ~OV2640(){
    };
    esp_err_t init(camera_config_t config);
    void done(void);
    esp_err_t run(void);
    size_t getSize(void);
    uint8_t *getfb(void);
    int getWidth(void);
    int getHeight(void);
    framesize_t getFrameSize(void);
    pixformat_t getPixelFormat(void);

private:
    void runIfNeeded(); // grab a frame if we don't already have one

    camera_config_t _cam_config;

    camera_fb_t *fb;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    int _jpg_width;
    int _jpg_height;
};

#endif //OV2640_H_

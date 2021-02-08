#include "esp_camera.h"
#include "string.h"
#include "soc/soc.h"          
#include "soc/rtc_cntl_reg.h"

#include "qrcode_recognize.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp32-hal-psram.h"
#include "Arduino.h"
#include <WiFi.h>
#include "quirc.h"
#include "quirc_internal.h"

#include "esp_http_server.h"
#include "esp_timer.h"
#include "img_converters.h"



#define CAMERA_MODEL_AI_THINKER
#define DEGUB_ESP

#ifdef DEGUB_ESP
  #define DBG(x) Serial.println(x)
#else 
  #define DBG(...)
#endif

#define PRINT_QR 1

static char *TAG = "QR-recognizer";

static const char *data_type_str(int dt)
{
    switch (dt) {
    case QUIRC_DATA_TYPE_NUMERIC:
        return "NUMERIC";
    case QUIRC_DATA_TYPE_ALPHA:
        return "ALPHA";
    case QUIRC_DATA_TYPE_BYTE:
        return "BYTE";
    case QUIRC_DATA_TYPE_KANJI:
        return "KANJI";
    }
    return "unknown";
}

using namespace std;

const char* ssid = "XXXXXXXX";
const char* password = "passwordhere";

camera_fb_t *fb = NULL;  
static camera_config_t camera_config;

bool qrMode = true;

// GPIO Setting
// Motor A = Right motor = OUT1 and OUT2
// Motor B = Left motor = OUT3 and OUT4

extern int Lb = 15; // Left Wheel Back 15
extern int Lf = 13; // Left Wheel Forward 13
extern int Rb = 14; // Right Wheel Back 14
extern int Rf = 2; // Right Wheel Forward 2
extern int LED =  4; // Light
extern String WiFiAddr = "";
String payload_master = "";

void startCameraServer();

// Set Static IP address
IPAddress local_IP(192, 168, 137, 20);
// Set Gateway IP address
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 0, 0);
IPAddress primaryDNS(8, 8, 8, 8); //optional
IPAddress secondaryDNS(8, 8, 4, 4); //optional

// QR Code functions

/*
static void print_payload() {
    printf("manual print payload: %s\n", payload_temp);
}*/


static void dump_data(const struct quirc_data *data)
{   
    char *payload_temp;
    payload_temp = reinterpret_cast<char*>(const_cast<uint8_t*>(data->payload)); //already a char
    /*
    printf("    Version: %d\n", data->version);
    printf("    ECC level: %c\n", "MLHQ"[data->ecc_level]);
    printf("    Mask: %d\n", data->mask);
    printf("    Data type: %d (%s)\n", data->data_type,
           data_type_str(data->data_type));
    printf("    Length: %d\n", data->payload_len);
    printf("\033[31m    Payload: %s\n", data->payload);

    if (data->eci) {
        printf("\033[31m    ECI: %d\n", data->eci);
    }
    printf("\033[0m\n");
    */
   int i;
   int cipher1 = payload_temp[0] - '0'; // - 0 converts char to int, + is from int to char
   int cipher1_op = payload_temp[1] - '0';
   int cipher2 = payload_temp[2] - '0';
   int cipher2_op = payload_temp[3] - '0';
   int cipher3 = payload_temp[4] - '0';
   int cipher3_op = payload_temp[5] - '0';

    for (i=6;i<strlen(payload_temp);i++) {
        if (cipher1_op % 2 == 0) {
            payload_temp[i] = payload_temp[i] - '0' - cipher1 + '0';
        } else {
            payload_temp[i] = payload_temp[i] - '0' + cipher1 + '0';
        }
    }

    for (i=6;i<strlen(payload_temp);i++) {
        if (cipher2_op % 2 == 0) {
            payload_temp[i] = payload_temp[i] - '0' - cipher2 + '0';
        } else {
            payload_temp[i] = payload_temp[i] - '0' + cipher2 + '0';
        }
    }

    for (i=6;i<strlen(payload_temp);i++) {
        if (cipher3_op % 2 == 0) {
            payload_temp[i] = payload_temp[i] - '0' - cipher3 + '0';
        } else {
            payload_temp[i] = payload_temp[i] - '0' + cipher3 + '0';
        }
    }

    //replace '_' with ' '
    for (i=0;i<strlen(payload_temp);i++) {
        if (payload_temp[i] == 95) {
            payload_temp[i] = 32;
        }
    }

   payload_master += payload_temp;
   payload_master += "<br>";
   
   printf("Payload: %s\n", payload_temp);
   printf("Length of payload_temp: %d\n", strlen(payload_temp));
   printf("%d\n", payload_temp[0]);
   printf("%d\n", payload_temp[1]);
   printf("%d\n", payload_temp[2]);



}

static void dump_info(struct quirc *q, uint8_t count)
{
    for (int i = 0; i < count; i++) {
        struct quirc_code code;
        struct quirc_data data; //declaring a quirc_data struct as data

        // Extract the QR-code specified by the given index.
        quirc_extract(q, i, &code);
        quirc_decode_error_t err = quirc_decode(&code, &data);
        dump_data(&data);        
    }
}

void qr_recoginze(void *parameter)
{   
    ESP_LOGE(TAG, "qr rec heap: %u", xPortGetFreeHeapSize());
    camera_config_t *camera_config = (camera_config_t *)parameter;
    // Use QVGA Size currently, but quirc can support other frame size.(eg: 
    // FRAMESIZE_QVGA,FRAMESIZE_HQVGA,FRAMESIZE_QCIF,FRAMESIZE_QQVGA2,FRAMESIZE_QQVGA,etc)
    if (camera_config->frame_size > FRAMESIZE_SVGA) {
        ESP_LOGE(TAG, "Camera Frame Size err %d, support maxsize is QVGA", (camera_config->frame_size));
        vTaskDelete(NULL);
    }

    struct quirc *qr_recognizer = NULL;
    camera_fb_t *fb = NULL; //initialises frame buffer
    uint8_t *image = NULL;
    int id_count = 0;

    // Save image width and height, avoid allocate memory repeatly.
    uint16_t old_width = 0;
    uint16_t old_height = 0;

    // Construct a new QR-code recognizer.
    ESP_LOGI(TAG, "Construct a new QR-code recognizer(quirc).");
    qr_recognizer = quirc_new();
    if (!qr_recognizer) {
        ESP_LOGE(TAG, "Can't create quirc object");
       
    }

    ESP_LOGE(TAG, "alloc qr heap: %u", xPortGetFreeHeapSize());
    ESP_LOGE(TAG, "uxHighWaterMark = %d", uxTaskGetStackHighWaterMark( NULL ));

    while (1) {
        // Capture a frame
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            esp_err_t esp_camera_deinit();
            esp_err_t err = esp_camera_init(camera_config);
            continue;
        }
        if (old_width != fb->width || old_height != fb->height) {
            ESP_LOGE(TAG, "Recognizer size change w h len: %d, %d, %d", fb->width, fb->height, fb->len);
            ESP_LOGE(TAG, "Resize the QR-code recognizer.");
            // Resize the QR-code recognizer.
            if (quirc_resize(qr_recognizer, fb->width, fb->height) < 0) {
                
                ESP_LOGE(TAG, "Resize the QR-code recognizer err (cannot allocate memory).");
                
                continue;
            } else {
                old_width = fb->width;
                old_height = fb->height;
            }
        }

        /** These functions are used to process images for QR-code recognition.
         * quirc_begin() must first be called to obtain access to a buffer into
         * which the input image should be placed. Optionally, the current
         * width and height may be returned.
         *
         * After filling the buffer, quirc_end() should be called to process
         * the image for QR-code recognition. The locations and content of each
         * code may be obtained using accessor functions described below.
         */

        image = quirc_begin(qr_recognizer, NULL, NULL); //quirc_begin obtains access to buffer
        memcpy(image, fb->buf, fb->len);    //assigns image to address of buf
        quirc_end(qr_recognizer);   //processes image for qr recognition

        // Return the number of QR-codes identified in the last processed image.
        id_count = quirc_count(qr_recognizer);
        if (id_count == 0) {
            esp_camera_fb_return(fb);
            continue;
        }
        

        // Print information of QR-code
        dump_info(qr_recognizer, id_count);
        //print_payload();
        esp_camera_fb_return(fb);
    }
    // Destroy QR-Code recognizer (quirc)
    quirc_destroy(qr_recognizer);
    ESP_LOGE(TAG, "Deconstruct QR-Code recognizer(quirc)");
    vTaskDelete(NULL);
}

void app_qr_recognize(void *pdata)
{
    xTaskCreate(qr_recoginze, "qr_recoginze", 1024 * 100, pdata, 5, NULL);
}
//---------------------------------------------------
void setup()
{
  #ifdef DEGUB_ESP
    Serial.begin(115200);
    Serial.setDebugOutput(true);
  #endif
  esp_log_level_set("*", ESP_LOG_VERBOSE);
  ESP_LOGE(TAG, "Free heap: %u", xPortGetFreeHeapSize());

  pinMode(Lb, OUTPUT); //Left Backward
  pinMode(Lf, OUTPUT); //Left Forward
  pinMode(Rb, OUTPUT); //Right Backwawrd
  pinMode(Rf, OUTPUT); //Right Forward
  pinMode(LED, OUTPUT); //Light

  //initialize
  digitalWrite(Lb, LOW);
  digitalWrite(Lf, LOW);
  digitalWrite(Rb, LOW);
  digitalWrite(Rf, LOW);
  digitalWrite(LED, LOW);


  //use lq camera settings if the module credentials are not set yet.

  camera_config.ledc_channel = LEDC_CHANNEL_0;
  camera_config.ledc_timer   = LEDC_TIMER_0;
  camera_config.pin_d0       = 5;
  camera_config.pin_d1       = 18;
  camera_config.pin_d2       = 19;
  camera_config.pin_d3       = 21;
  camera_config.pin_d4       = 36;
  camera_config.pin_d5       = 39;
  camera_config.pin_d6       = 34;
  camera_config.pin_d7       = 35;
  camera_config.pin_xclk     = 0;
  camera_config.pin_pclk     = 22;
  camera_config.pin_vsync    = 25;
  camera_config.pin_href     = 23;
  camera_config.pin_sscb_sda = 26;
  camera_config.pin_sscb_scl = 27;
  camera_config.pin_pwdn     = 32;
  camera_config.pin_reset    = -1;
  camera_config.xclk_freq_hz = 20000000;
  camera_config.pixel_format = PIXFORMAT_GRAYSCALE;
  
  camera_config.frame_size = FRAMESIZE_QCIF;  // set picture size, FRAMESIZE_VGA (640x480)
  camera_config.jpeg_quality = 15;           // quality of JPEG output. 0-63 lower means higher quality
  camera_config.fb_count = 1;              // 1: Wait for V-Synch // 2: Continous Capture (Video)

  //Use hq camera settings once the module is credentials are set.

  esp_err_t err = esp_camera_init(&camera_config);
  if (err != ESP_OK)
  {
    Serial.println("Camera init failed with error 0x%x");
    Serial.println(err);
    
    delay(3000);
    ESP.restart();
    return;
  }
    if(psramFound())
    {
      Serial.println("PSRAM found and loaded");
    }
    ESP_LOGE(TAG, "Free heap after setup: %u", xPortGetFreeHeapSize());
    Serial.println("Setup done.");

  if (!WiFi.config(local_IP, gateway, subnet)) {
    Serial.println("STA Failed to configure");
  }
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED, HIGH);
    delay(250);
    digitalWrite(LED, LOW);
    delay(250);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  startCameraServer();

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  WiFiAddr = WiFi.localIP().toString();
  Serial.println("' to connect");

}

//---------------------------HTML FUNCTION-------------------------
void Direction(int dirLf, int dirLb, int dirRf, int dirRb);

typedef struct {
        size_t size; //number of values used for filtering
        size_t index; //current value index
        size_t count; //value count
        int sum;
        int * values; //array to be filled with values
} ra_filter_t;

typedef struct {
        httpd_req_t *req;
        size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static ra_filter_t ra_filter;
httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

static ra_filter_t * ra_filter_init(ra_filter_t * filter, size_t sample_size){
    memset(filter, 0, sizeof(ra_filter_t));

    filter->values = (int *)malloc(sample_size * sizeof(int));
    if(!filter->values){
        return NULL;
    }
    memset(filter->values, 0, sample_size * sizeof(int));

    filter->size = sample_size;
    return filter;
}

static int ra_filter_run(ra_filter_t * filter, int value){
    if(!filter->values){
        return value;
    }
    filter->sum -= filter->values[filter->index];
    filter->values[filter->index] = value;
    filter->sum += filter->values[filter->index];
    filter->index++;
    filter->index = filter->index % filter->size;
    if (filter->count < filter->size) {
        filter->count++;
    }
    return filter->sum / filter->count;
}

static size_t jpg_encode_stream(void * arg, size_t index, const void* data, size_t len){
    jpg_chunking_t *j = (jpg_chunking_t *)arg;
    if(!index){
        j->len = 0;
    }
    if(httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK){
        return 0;
    }
    j->len += len;
    return len;
}

static esp_err_t capture_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    int64_t fr_start = esp_timer_get_time();

    fb = esp_camera_fb_get();
    if (!fb) {
        Serial.printf("Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");

    size_t fb_len = 0;
    if(fb->format == PIXFORMAT_JPEG){
        fb_len = fb->len;
        res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    } else {
        jpg_chunking_t jchunk = {req, 0};
        res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk)?ESP_OK:ESP_FAIL;
        httpd_resp_send_chunk(req, NULL, 0);
        fb_len = jchunk.len;
    }
    esp_camera_fb_return(fb);
    int64_t fr_end = esp_timer_get_time();
    Serial.printf("JPG: %uB %ums", (uint32_t)(fb_len), (uint32_t)((fr_end - fr_start)/1000));
    return res;
}

static esp_err_t stream_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t * _jpg_buf = NULL;
    char * part_buf[64];

    static int64_t last_frame = 0;
    if(!last_frame) {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        return res;
    }

    while(true){
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.printf("Camera capture failed");
            res = ESP_FAIL;
        } else {
            if(fb->format != PIXFORMAT_JPEG){
                bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
                esp_camera_fb_return(fb);
                fb = NULL;
                if(!jpeg_converted){
                    Serial.printf("JPEG compression failed");
                    res = ESP_FAIL;
                }
            } else {
                _jpg_buf_len = fb->len;
                _jpg_buf = fb->buf;
            }
        }
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(fb){
            esp_camera_fb_return(fb);
            fb = NULL;
            _jpg_buf = NULL;
        } else if(_jpg_buf){
            free(_jpg_buf);
            _jpg_buf = NULL;
        }
        if(res != ESP_OK){
            break;
        }
        int64_t fr_end = esp_timer_get_time();

        int64_t frame_time = fr_end - last_frame;
        last_frame = fr_end;
        frame_time /= 1000;
        uint32_t avg_frame_time = ra_filter_run(&ra_filter, frame_time);
        /*
        Serial.printf("MJPG: %uB %ums (%.1ffps), AVG: %ums (%.1ffps)"
            ,(uint32_t)(_jpg_buf_len),
            (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time,
            avg_frame_time, 1000.0 / avg_frame_time
        );
        */
    }

    last_frame = 0;
    return res;
}

static esp_err_t cmd_handler(httpd_req_t *req){
    char*  buf;
    size_t buf_len;
    char variable[32] = {0,};
    char value[32] = {0,};

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = (char*)malloc(buf_len);
        if(!buf){
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) == ESP_OK &&
                httpd_query_key_value(buf, "val", value, sizeof(value)) == ESP_OK) {
            } else {
                free(buf);
                httpd_resp_send_404(req);
                return ESP_FAIL;
            }
        } else {
            free(buf);
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }
        free(buf);
    } else {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    int val = atoi(value);
    sensor_t * s = esp_camera_sensor_get();
    int res = 0;

    if(!strcmp(variable, "framesize")) {
        if(s->pixformat == PIXFORMAT_JPEG) res = s->set_framesize(s, (framesize_t)val);
    }
    else if(!strcmp(variable, "quality")) res = s->set_quality(s, val);
    else if(!strcmp(variable, "contrast")) res = s->set_contrast(s, val);
    else if(!strcmp(variable, "brightness")) res = s->set_brightness(s, val);
    else if(!strcmp(variable, "saturation")) res = s->set_saturation(s, val);
    else if(!strcmp(variable, "gainceiling")) res = s->set_gainceiling(s, (gainceiling_t)val);
    else if(!strcmp(variable, "colorbar")) res = s->set_colorbar(s, val);
    else if(!strcmp(variable, "awb")) res = s->set_whitebal(s, val);
    else if(!strcmp(variable, "agc")) res = s->set_gain_ctrl(s, val);
    else if(!strcmp(variable, "aec")) res = s->set_exposure_ctrl(s, val);
    else if(!strcmp(variable, "hmirror")) res = s->set_hmirror(s, val);
    else if(!strcmp(variable, "vflip")) res = s->set_vflip(s, val);
    else if(!strcmp(variable, "awb_gain")) res = s->set_awb_gain(s, val);
    else if(!strcmp(variable, "agc_gain")) res = s->set_agc_gain(s, val);
    else if(!strcmp(variable, "aec_value")) res = s->set_aec_value(s, val);
    else if(!strcmp(variable, "aec2")) res = s->set_aec2(s, val);
    else if(!strcmp(variable, "dcw")) res = s->set_dcw(s, val);
    else if(!strcmp(variable, "bpc")) res = s->set_bpc(s, val);
    else if(!strcmp(variable, "wpc")) res = s->set_wpc(s, val);
    else if(!strcmp(variable, "raw_gma")) res = s->set_raw_gma(s, val);
    else if(!strcmp(variable, "lenc")) res = s->set_lenc(s, val);
    else if(!strcmp(variable, "special_effect")) res = s->set_special_effect(s, val);
    else if(!strcmp(variable, "wb_mode")) res = s->set_wb_mode(s, val);
    else if(!strcmp(variable, "ae_level")) res = s->set_ae_level(s, val);
    else {
        res = -1;
    }

    if(res){
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_handler(httpd_req_t *req){
    static char json_response[1024];

    sensor_t * s = esp_camera_sensor_get();
    char * p = json_response;
    *p++ = '{';

    p+=sprintf(p, "\"framesize\":%u,", s->status.framesize);
    p+=sprintf(p, "\"quality\":%u,", s->status.quality);
    p+=sprintf(p, "\"brightness\":%d,", s->status.brightness);
    p+=sprintf(p, "\"contrast\":%d,", s->status.contrast);
    p+=sprintf(p, "\"saturation\":%d,", s->status.saturation);
    p+=sprintf(p, "\"special_effect\":%u,", s->status.special_effect);
    p+=sprintf(p, "\"wb_mode\":%u,", s->status.wb_mode);
    p+=sprintf(p, "\"awb\":%u,", s->status.awb);
    p+=sprintf(p, "\"awb_gain\":%u,", s->status.awb_gain);
    p+=sprintf(p, "\"aec\":%u,", s->status.aec);
    p+=sprintf(p, "\"aec2\":%u,", s->status.aec2);
    p+=sprintf(p, "\"ae_level\":%d,", s->status.ae_level);
    p+=sprintf(p, "\"aec_value\":%u,", s->status.aec_value);
    p+=sprintf(p, "\"agc\":%u,", s->status.agc);
    p+=sprintf(p, "\"agc_gain\":%u,", s->status.agc_gain);
    p+=sprintf(p, "\"gainceiling\":%u,", s->status.gainceiling);
    p+=sprintf(p, "\"bpc\":%u,", s->status.bpc);
    p+=sprintf(p, "\"wpc\":%u,", s->status.wpc);
    p+=sprintf(p, "\"raw_gma\":%u,", s->status.raw_gma);
    p+=sprintf(p, "\"lenc\":%u,", s->status.lenc);
    p+=sprintf(p, "\"hmirror\":%u,", s->status.hmirror);
    p+=sprintf(p, "\"dcw\":%u,", s->status.dcw);
    p+=sprintf(p, "\"colorbar\":%u", s->status.colorbar);
    *p++ = '}';
    *p++ = 0;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_response, strlen(json_response));
}

char temp[1000];
int buf = 0;

static esp_err_t index_handler(httpd_req_t *req){
    httpd_resp_set_type(req, "text/html");
    String page = "";
    /*
    buf += strlen(payload_temp);
    strcpy(temp, payload_temp);
    printf("temp is %s", temp);
    printf("buf is %d", buf);
    */
    page += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=0\">";
    page += "<script>var xhttp = new XMLHttpRequest();</script>";
    page += "<script>function getsend(arg) { xhttp.open('GET', arg +'?' + new Date().getTime(), true); xhttp.send() } </script>";
    page += "<div style='min-width:500px; max-width:1200px;margin:0 auto;'>";
    page += "<div style='display:block; text-align:center;' id='image-container'>";
    page += "<div style='display:inline-block; height:200px; width:200px; text-align: center;'><IMG SRC='http://" + WiFiAddr + ":81/stream' style='width:200px; height:200px; transform:rotate(90deg);'><br/><br/></div>";
    page += "<div style='display:inline-block; height:200px; width:200px; text-align: center;'><IMG SRC='http://192.168.137.10:81/stream' style='width:200px; height:200px; transform:rotate(90deg);'><br/><br/></div>";
    page += "</div>";
    page += "<div style='float:left; width:49%;'>";
    page += "<p align=center><button style=width:60px;height:60px onmousedown=getsend('go') onmouseup=getsend('stop') ontouchstart=getsend('go') ontouchend=getsend('stop') ></button></p>";
    page += "<p align=center><button style=width:60px;height:60px onmousedown=getsend('left') onmouseup=getsend('stop') ontouchstart=getsend('left') ontouchend=getsend('stop')></button>&nbsp;";
    page += "<button style=width:60px;height:60px onmousedown=getsend('stop') onmouseup=getsend('stop')></button>&nbsp;";
    page += "<button style=width:60px;height:60px onmousedown=getsend('right') onmouseup=getsend('stop') ontouchstart=getsend('right') ontouchend=getsend('stop')></button></p>";
    page += "<p align=center><button style=width:60px;height:60px onmousedown=getsend('back') onmouseup=getsend('stop') ontouchstart=getsend('back') ontouchend=getsend('stop') ></button></p>";
    page += "<p align=center><button style=width:140px;height:40px onmousedown=getsend('ledon')>LED ON</button>";
    page += "<button style=width:140px;height:40px onmousedown=getsend('ledoff')>LED OFF</button></p>";
    page += "</div><div style='min-height: 500px; width:49%; float:left;'>";   
    page += "<div id='qrcontainer' style='margin:0 20px;'>";
    page += "<p style='padding: 0 20px;'><b>Your scanned QR codes will appear here!</b></p>";
    page += "<button style='margin:0 20px;'id='refreshqr'>Click to update QR!</button>";
    page += "<div id='qrcodes'>";
    page += "<p style='padding: 0 20px;'>";
    page += payload_master + "</p>";
    page += "<br></div></div></div></div>";
    page += "<script>var qrbutton = document.getElementById('refreshqr');";
    page += "qrbutton.onclick = function(){location.reload()};";
    page += "qrbutton.ontouchstart = function(){location.reload()};";
    page += "</script>";
    return httpd_resp_send(req, &page[0], strlen(&page[0]));
}

static esp_err_t go_handler(httpd_req_t *req){
    Direction(HIGH, LOW, HIGH, LOW);
    Serial.println("Go");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, "OK", 2);
}
static esp_err_t back_handler(httpd_req_t *req){
    Direction(LOW, HIGH, LOW, HIGH);
    Serial.println("Back");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, "OK", 2);
}

static esp_err_t left_handler(httpd_req_t *req){
    Direction(HIGH, LOW, LOW, HIGH);
    Serial.println("Left");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, "OK", 2);
}
static esp_err_t right_handler(httpd_req_t *req){
    Direction(LOW, HIGH, HIGH, LOW);
    Serial.println("Right");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, "OK", 2);
}

static esp_err_t stop_handler(httpd_req_t *req){
    Direction(LOW, LOW, LOW, LOW);
    Serial.println("Stop");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, "OK", 2);
}

static esp_err_t ledon_handler(httpd_req_t *req){
    digitalWrite(LED, HIGH);
    Serial.println("LED ON");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, "OK", 2);
}
static esp_err_t ledoff_handler(httpd_req_t *req){
    digitalWrite(LED, LOW);
    Serial.println("LED OFF");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, "OK", 2);
}


void startCameraServer(){
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_uri_t go_uri = {
        .uri       = "/go",
        .method    = HTTP_GET,
        .handler   = go_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t back_uri = {
        .uri       = "/back",
        .method    = HTTP_GET,
        .handler   = back_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t stop_uri = {
        .uri       = "/stop",
        .method    = HTTP_GET,
        .handler   = stop_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t left_uri = {
        .uri       = "/left",
        .method    = HTTP_GET,
        .handler   = left_handler,
        .user_ctx  = NULL
    };
    
    httpd_uri_t right_uri = {
        .uri       = "/right",
        .method    = HTTP_GET,
        .handler   = right_handler,
        .user_ctx  = NULL
    };
    
    httpd_uri_t ledon_uri = {
        .uri       = "/ledon",
        .method    = HTTP_GET,
        .handler   = ledon_handler,
        .user_ctx  = NULL
    };
    
    httpd_uri_t ledoff_uri = {
        .uri       = "/ledoff",
        .method    = HTTP_GET,
        .handler   = ledoff_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t index_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = index_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t status_uri = {
        .uri       = "/status",
        .method    = HTTP_GET,
        .handler   = status_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t cmd_uri = {
        .uri       = "/control",
        .method    = HTTP_GET,
        .handler   = cmd_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t capture_uri = {
        .uri       = "/capture",
        .method    = HTTP_GET,
        .handler   = capture_handler,
        .user_ctx  = NULL
    };

   httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };


    ra_filter_init(&ra_filter, 20);
    Serial.printf("Starting web server on port: '%d'", config.server_port);
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        //httpd_register_uri_handler(camera_httpd, &index_uri);
        httpd_register_uri_handler(camera_httpd, &go_uri); 
        httpd_register_uri_handler(camera_httpd, &back_uri); 
        httpd_register_uri_handler(camera_httpd, &stop_uri); 
        httpd_register_uri_handler(camera_httpd, &left_uri);
        httpd_register_uri_handler(camera_httpd, &right_uri);
        httpd_register_uri_handler(camera_httpd, &ledon_uri);
        httpd_register_uri_handler(camera_httpd, &ledoff_uri);
    }

    config.server_port += 1;
    config.ctrl_port += 1;
    Serial.printf("Starting stream server on port: '%d'", config.server_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
    }
}

void Direction(int dirLf, int dirLb, int dirRf, int dirRb)
{
 digitalWrite(Lf, dirLf);
 digitalWrite(Lb, dirLb);
 digitalWrite(Rf, dirRf);
 digitalWrite(Rb, dirRb);
}

//---------------------------QR FUNCTION-------------------------

void qr()
{
  bool started = false;
  
  if (started == false) 
  {
  app_qr_recognize(&camera_config);
  started = true;
  } 

}

httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler,
    .user_ctx  = NULL
};

//----------------------------------- LOOP  --------------------
void loop()
{
    httpd_register_uri_handler(camera_httpd, &index_uri);
    qr();
    /*
    WiFiClient client = 1;
    client.println("HTTP/1.1 200 OK");
    client.println("Content-type:text/html");
    client.println("Connection: close");
    client.println(payload_temp);
    */
    
}
#include <filesystem>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <list>
#include <ndi.h>
#include <numeric>
#include <sgct/opengl.h>
#include <sgct/sgct.h>
#include <sgct/utils/dome.h>
#include <thread>

using namespace sgct;

uint64_t frameNumber = 0;
float phi = glm::pi<float>();
float theta = 0.f;
int renderView = 2;
bool showId = false;
bool showStats = false;
ndi_frame_t frame;
ndi_video_format_t format;
ndi_codec_context_t codec_ctx;
std::list<ndi_packet_video_t *> _queue;
std::mutex _mutex;
bool videochanged;
GLuint yuvtexture1 = 0;
GLuint yuvtexture2 = 0;
GLuint yuvtexture3 = 0;
sgct::Image yuvvideoframe1;
sgct::Image yuvvideoframe2;
sgct::Image yuvvideoframe3;
std::unique_ptr<sgct::utils::Dome> dome;
GLint domeMvpMatrixLoc = -1;
GLint domeCameraMatrixLoc = -1;

constexpr std::string_view DomeVertexShader = R"(
  #version 330 core

  layout(location = 0) in vec2 texCoords;
  layout(location = 1) in vec3 normals;
  layout(location = 2) in vec3 vertPositions;

  uniform mat4 mvp;
  uniform mat4 camera;
  out vec2 uv;

  void main() {
    gl_Position =  mvp * camera * vec4(vertPositions, 1.0);
    uv = texCoords;
  })";

constexpr std::string_view DomeFragmentShader = R"(
  #version 330 core

  uniform sampler2D tex1;
  uniform sampler2D tex2;
  uniform sampler2D tex3;

  vec3 offset = vec3(-0.0625, -0.5, -0.5);
  vec3 rcoeff = vec3(1.164, 0.000, 1.596);
  vec3 gcoeff = vec3(1.164, -0.391, -0.813);
  vec3 bcoeff = vec3(1.164, 2.018, 0.000);

  in vec2 uv;
  out vec4 color;

  void main() {
    vec4 t1 = texture(tex1, uv);
    vec4 t2 = texture(tex2, uv);
    vec4 t3 = texture(tex3, uv);
    vec3 yuv = vec3(t1.x, t2.x, t3.x) + offset;
    float r = dot(yuv, rcoeff);
    float g = dot(yuv, gcoeff);
    float b = dot(yuv, bcoeff);

    color = vec4(r, g, b, 1.0);
  }
)";

void recv_thread(ndi_recv_context_t recv_ctx) {
  while (true) {

    // NDI find source
    ndi_find_context_t find_ctx = ndi_find_create();
    ndi_source_t *sources = NULL;
    int nb_sources = 0;
    while (!nb_sources) {
      printf("Looking for sources ...\n");
      sources = ndi_find_sources(find_ctx, 5000, &nb_sources);
    }

    printf("Found source: %s (%s:%d)\n", sources[0].name, sources[0].ip,
           sources[0].port);

    // NDI receive
    ndi_recv_context_t recv_ctx = ndi_recv_create();
    int ret = ndi_recv_connect(recv_ctx, sources[0].ip, sources[0].port);
    if (ret < 0) {
      printf("Failed to connect to source\n");
    }
    printf("Connected.\n");

    ndi_find_free(find_ctx);

    ndi_packet_video_t video;
    ndi_packet_audio_t audio;
    ndi_packet_metadata_t meta;

    while (ndi_recv_is_connected(recv_ctx)) {

      int data_type = ndi_recv_capture(recv_ctx, &video, &audio, &meta, 1000);
      switch (data_type) {

      case NDI_DATA_TYPE_VIDEO:
        // printf("Video data received (%dx%d %.4s).\n", video.width,
        // video.height, (char*)&video.fourcc);
        {
          ndi_packet_video_t *clone =
              (ndi_packet_video_t *)malloc(sizeof(ndi_packet_video_t));
          memcpy(clone, &video, sizeof(ndi_packet_video_t));
          _mutex.lock();
          while (_queue.size() > 2) {
            ndi_packet_video_t *v = _queue.back();
            _queue.pop_back();
            ndi_recv_free_video(v);
            free(v);
          }
          _queue.push_back(clone);
          _mutex.unlock();
        }
        break;

      case NDI_DATA_TYPE_AUDIO:
        // printf("Audio data received (%d samples).\n", audio.num_samples);
        ndi_recv_free_audio(&audio);
        break;

      case NDI_DATA_TYPE_METADATA:
        printf("Meta data received: %s\n", meta.data);
        ndi_recv_free_metadata(&meta);
        break;
      }
    }

    printf("NDI disconnected.\n");
  }
}

void initGL(GLFWwindow *) {
  ShaderManager::instance().addShaderProgram("xform", DomeVertexShader,
                                             DomeFragmentShader);
  const ShaderProgram &prog2 = ShaderManager::instance().shaderProgram("xform");
  prog2.bind();
  domeCameraMatrixLoc = glGetUniformLocation(prog2.id(), "camera");
  domeMvpMatrixLoc = glGetUniformLocation(prog2.id(), "mvp");
  glUniform1i(glGetUniformLocation(prog2.id(), "tex1"), 0);
  glUniform1i(glGetUniformLocation(prog2.id(), "tex2"), 1);
  glUniform1i(glGetUniformLocation(prog2.id(), "tex3"), 2);
  prog2.unbind();

  dome = std::make_unique<utils::Dome>(7.4f, 180.f, 256, 128);

  struct ImageData {
    std::string filename;
    Image img;
    std::atomic_bool imageDone = false;
    std::atomic_bool uploadDone = false;
    std::atomic_bool threadDone = false;
  };
  auto loadImage = [](ImageData &data) {
    data.img.load(data.filename);
    data.imageDone = true;
    while (!data.uploadDone) {
    }
    data.img = Image();
    data.threadDone = true;
  };

  Log::Info("Loading placeholder graphics...");

  yuvvideoframe1.load("empty_y.png");
  yuvvideoframe2.load("empty_u.png");
  yuvvideoframe3.load("empty_v.png");

  yuvtexture1 = TextureManager::instance().loadTexture(yuvvideoframe1);
  yuvtexture2 = TextureManager::instance().loadTexture(yuvvideoframe2);
  yuvtexture3 = TextureManager::instance().loadTexture(yuvvideoframe3);
}

void postSyncPreDraw() {
  Engine::instance().setStatsGraphVisibility(showStats);
}

void draw(const RenderData &data) {

  // Pop most recent frame off the queue
  ndi_packet_video_t *video = NULL;
  if (_queue.size() > 0) {
    _mutex.lock();
    video = _queue.front();
    _queue.pop_front();
    _mutex.unlock();
  }

  // Decode video if available
  if (video != NULL) {
    frame = ndi_codec_decode(codec_ctx, video);
    if (frame) {
      ndi_frame_get_format(frame, &format);

      // printf("got frame: %d planes, %dx%d, chroma %dx%d\n",
      // format.num_planes,
      //        format.width, format.height, format.chroma_width,
      //        format.chroma_height);

      for (int i = 0; i < format.num_planes; i++) {
        void *data = ndi_frame_get_data(frame, i);
        int w = i ? format.chroma_width : format.width;
        int h = i ? format.chroma_height : format.height;
        int linesize = ndi_frame_get_linesize(frame, i);
        w = linesize;

        if (i == 0) {
          if (w != yuvvideoframe1.size().x || h != yuvvideoframe1.size().y) {
            printf("resizing yuvvideoframe1 to %dx%d\n", w, h);
            yuvvideoframe1.setSize(ivec2(w, h));
            yuvvideoframe1.allocateOrResizeData();
          }
          uint8_t *ptr = yuvvideoframe1.data();
          memcpy(ptr, (unsigned char *)data, w * h);
          videochanged = true;
        }

        if (i == 1) {
          if (w != yuvvideoframe2.size().x || h != yuvvideoframe2.size().y) {
            printf("resizing yuvvideoframe2 to %dx%d\n", w, h);
            yuvvideoframe2.setSize(ivec2(w, h));
            yuvvideoframe2.allocateOrResizeData();
          }
          uint8_t *ptr = yuvvideoframe2.data();
          memcpy(ptr, (unsigned char *)data, w * h);
          videochanged = true;
        }

        if (i == 2) {
          if (w != yuvvideoframe3.size().x || h != yuvvideoframe3.size().y) {
            printf("resizing yuvvideoframe3 to %dx%d\n", w, h);
            yuvvideoframe3.setSize(ivec2(w, h));
            yuvvideoframe3.allocateOrResizeData();
          }
          uint8_t *ptr = yuvvideoframe3.data();
          memcpy(ptr, (unsigned char *)data, w * h);
          videochanged = true;
        }
      }
      ndi_frame_free(frame);
    }
    ndi_recv_free_video(video);
    free(video);
  }

  if (videochanged) {
    GLenum format = GL_UNSIGNED_BYTE;
    GLenum type = GL_RED;          //  GL_BGRA;
    GLenum internalFormat = GL_R8; // GL_RGBA8;

    glBindTexture(GL_TEXTURE_2D, yuvtexture1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, yuvvideoframe1.size().x,
                 yuvvideoframe1.size().y, 0, type, format,
                 yuvvideoframe1.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindTexture(GL_TEXTURE_2D, yuvtexture2);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, yuvvideoframe2.size().x,
                 yuvvideoframe2.size().y, 0, type, format,
                 yuvvideoframe2.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindTexture(GL_TEXTURE_2D, yuvtexture3);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, yuvvideoframe3.size().x,
                 yuvvideoframe3.size().y, 0, type, format,
                 yuvvideoframe3.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    videochanged = false;
  }

  const mat4 mvp = data.modelViewProjectionMatrix;

  glm::vec3 direction = {std::cos(theta) * std::sin(phi), std::sin(theta),
                         std::cos(theta) * std::cos(phi)};
  glm::vec3 right = {std::sin(phi - glm::half_pi<float>()), 0.f,
                     std::cos(phi - glm::half_pi<float>())};
  glm::vec3 up = glm::cross(right, direction);
  const glm::mat4 c = glm::lookAt(glm::vec3(0.f), direction, up);

  if (renderView == 2) {
    ShaderManager::instance().shaderProgram("xform").bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, yuvtexture1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, yuvtexture2);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, yuvtexture3);
    glUniformMatrix4fv(domeMvpMatrixLoc, 1, GL_FALSE, mvp.values);
    glUniformMatrix4fv(domeCameraMatrixLoc, 1, GL_FALSE, glm::value_ptr(c));
    dome->draw();
    ShaderManager::instance().shaderProgram("xform").unbind();
  }
}

std::vector<std::byte> encode() {
  std::vector<std::byte> data;
  serializeObject(data, renderView);
  serializeObject(data, frameNumber);
  serializeObject(data, showId);
  serializeObject(data, showStats);
  serializeObject(data, theta);
  serializeObject(data, phi);
  return data;
}

void decode(const std::vector<std::byte> &data) {
  unsigned pos = 0;
  deserializeObject(data, pos, renderView);
  deserializeObject(data, pos, frameNumber);
  deserializeObject(data, pos, showId);
  deserializeObject(data, pos, showStats);
  deserializeObject(data, pos, theta);
  deserializeObject(data, pos, phi);
}

void cleanup() {
  TextureManager::instance().removeTexture(yuvtexture1);
  TextureManager::instance().removeTexture(yuvtexture2);
  TextureManager::instance().removeTexture(yuvtexture3);
}

void keyboard(Key key, Modifier, Action action, int, Window *) {
  if (key == Key::Space && action == Action::Press) {
    renderView = (renderView + 1) % 3;
  }

  if (key == Key::Enter && action == Action::Press) {
    frameNumber = 0;
  }

  if (key == Key::Esc && action == Action::Press) {
    Engine::instance().terminate();
  }

  if (key == Key::I && action == Action::Press) {
    showId = !showId;
  }

  if (key == Key::S && action == Action::Press) {
    showStats = !showStats;
  }

  if (key == Key::Left &&
      (action == Action::Press || action == Action::Repeat)) {
    phi += 0.1f;
    if (phi > glm::two_pi<float>()) {
      phi -= glm::two_pi<float>();
    }
  }

  if (key == Key::Right &&
      (action == Action::Press || action == Action::Repeat)) {
    phi -= 0.1f;
    if (phi < -glm::two_pi<float>()) {
      phi += glm::two_pi<float>();
    }
  }

  if (key == Key::Down &&
      (action == Action::Press || action == Action::Repeat)) {
    theta -= 0.1f;
    theta = std::clamp(theta, -glm::half_pi<float>(), glm::half_pi<float>());
  }

  if (key == Key::Up && (action == Action::Press || action == Action::Repeat)) {
    theta += 0.1f;
    theta = std::clamp(theta, -glm::half_pi<float>(), glm::half_pi<float>());
  }
}

int main(int argc, char **argv) {
  int ret;
  uint8_t *ptr;

  std::vector<std::string> arg(argv + 1, argv + argc);
  Configuration config = parseArguments(arg);
  config::Cluster cluster = loadCluster(config.configFilename);
  if (!cluster.success) {
    return -1;
  }

  ndi_recv_context_t recv_ctx = ndi_recv_create();
  codec_ctx = ndi_codec_create();
  std::thread t(recv_thread, recv_ctx);

  Engine::Callbacks callbacks;
  callbacks.initOpenGL = initGL;
  callbacks.encode = encode;
  callbacks.decode = decode;
  callbacks.postSyncPreDraw = postSyncPreDraw;
  callbacks.draw = draw;
  callbacks.cleanup = cleanup;
  callbacks.keyboard = keyboard;

  try {
    Engine::create(cluster, callbacks, config);
  } catch (const std::runtime_error &e) {
    Log::Error(e.what());
    Engine::destroy();
    return EXIT_FAILURE;
  }

  Log::Info("===========");
  Log::Info("Keybindings");
  Log::Info("LEFT:  Move camera pointing to the left");
  Log::Info("RIGHT: Move camera pointing to the right");
  Log::Info("UP:    Move camera pointing to up");
  Log::Info("DOWN:  Move camera pointing to down");
  Log::Info("ESC:   Terminate the program");
  Log::Info("I:     Show node id and IP");
  Log::Info("S:     Show statistics graphs");
  Log::Info("===========");

  Engine::instance().exec();
  Engine::destroy();

  exit(EXIT_SUCCESS);
}

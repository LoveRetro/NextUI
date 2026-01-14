/*
 * show2.elf - Enhanced splash screen / loading screen tool
 * 
 * Modes:
 *   1. Simple image display (centered logo, runs until killed)
 *   2. Logo + progress bar + text
 *   3. Daemon mode for runtime updates via FIFO
 *
 * Usage:
 *   show2.elf --mode=simple --image=<path> [--bgcolor=0x000000]
 *   show2.elf --mode=progress --image=<path> [--bgcolor=0x000000] [--fontcolor=0xFFFFFF] [--text="message"] [--progress=0]
 *   show2.elf --mode=daemon --image=<path> [--bgcolor=0x000000] [--fontcolor=0xFFFFFF] [--text="message"]
 * 
 * Daemon mode FIFO commands (/tmp/show2.fifo):
 *   TEXT:message
 *   PROGRESS:50
 *   BGCOLOR:0xRRGGBB
 *   QUIT
 */

#include <iostream>
#include <string>
#include <map>
#include <memory>
#include <cstring>
#include <cmath>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include "embedded_font_rounded.h"

constexpr const char* FIFO_PATH = "/tmp/show2.fifo";
constexpr int MAX_TEXT_LEN = 256;
constexpr int PROGRESS_BAR_HEIGHT = 10;
constexpr int PROGRESS_BAR_WIDTH = 400;
constexpr int TEXT_PADDING = 20;
constexpr int LOGO_TEXT_PADDING = 40;
constexpr int FONT_SIZE = 24;
constexpr int FPS = 60;

enum class DisplayMode {
    Simple,
    Progress,
    Daemon
};

struct Config {
    DisplayMode mode = DisplayMode::Simple;
    std::string image_path;
    uint32_t bg_color_rgb = 0x000000;
    uint32_t font_color_rgb = 0xFFFFFF;
    std::string text;
    int progress = 0;
    int text_y_pct = 80;      // Text Y position as percentage of screen height
    int progress_y_pct = 90;  // Progress bar Y position as percentage of screen height
    int timeout_seconds = 0;  // Auto-close timeout (0 = no timeout)
    int logo_height = 0;      // Scale logo to this height (0 = no scaling)
};

class ShowApp {
private:
    SDL_Window* window = nullptr;
    SDL_Surface* screen = nullptr;
    SDL_Surface* logo = nullptr;
    SDL_Surface* scaled_logo = nullptr;
    TTF_Font* font = nullptr;
    Config config;
    uint32_t bg_color_sdl = 0;
    SDL_Color font_color;
    std::string current_text;
    int current_progress = 0;
    int current_text_y_pct = 80;
    int current_progress_y_pct = 90;
    float indeterminate_pos = 0.0f;  // Position for indeterminate animation (0.0 to 1.0)
    bool indeterminate_forward = true;
    uint32_t start_time = 0;  // SDL ticks when app started
    bool running = true;
    pthread_mutex_t mutex;
    pthread_t fifo_thread_handle;

public:
    ShowApp(const Config& cfg) : config(cfg) {
        pthread_mutex_init(&mutex, nullptr);
        current_text = config.text;
        current_progress = config.progress;
        current_text_y_pct = config.text_y_pct;
        current_progress_y_pct = config.progress_y_pct;
        font_color.r = (config.font_color_rgb >> 16) & 0xFF;
        font_color.g = (config.font_color_rgb >> 8) & 0xFF;
        font_color.b = config.font_color_rgb & 0xFF;
        font_color.a = 255;
    }

    ~ShowApp() {
        cleanup();
        pthread_mutex_destroy(&mutex);
    }

    bool initialize() {
        // Initialize SDL
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
            return false;
        }

        SDL_ShowCursor(0);

        window = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                   0, 0, SDL_WINDOW_SHOWN);
        if (!window) {
            std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
            SDL_Quit();
            return false;
        }

        screen = SDL_GetWindowSurface(window);
        if (!screen) {
            std::cerr << "SDL_GetWindowSurface failed: " << SDL_GetError() << std::endl;
            return false;
        }

        // Set background color
        bg_color_sdl = SDL_MapRGB(screen->format,
                                  (config.bg_color_rgb >> 16) & 0xFF,
                                  (config.bg_color_rgb >> 8) & 0xFF,
                                  config.bg_color_rgb & 0xFF);

        // Load image
        if (access(config.image_path.c_str(), F_OK) == 0) {
            logo = IMG_Load(config.image_path.c_str());
            if (!logo) {
                std::cerr << "IMG_Load failed: " << IMG_GetError() << std::endl;
            } else if (config.logo_height > 0 && logo->h != config.logo_height) {
                // Scale logo to specified height, maintaining aspect ratio
                float scale = (float)config.logo_height / logo->h;
                int new_width = (int)(logo->w * scale);
                int new_height = config.logo_height;
                
                // Create a new surface with the target dimensions
                scaled_logo = SDL_CreateRGBSurface(0, new_width, new_height, 
                                                   logo->format->BitsPerPixel,
                                                   logo->format->Rmask,
                                                   logo->format->Gmask,
                                                   logo->format->Bmask,
                                                   logo->format->Amask);
                if (scaled_logo) {
                    // Scale using software rendering
                    SDL_Rect dst_rect = {0, 0, new_width, new_height};
                    SDL_BlitScaled(logo, nullptr, scaled_logo, &dst_rect);
                    SDL_FreeSurface(logo);
                    logo = scaled_logo;
                    scaled_logo = nullptr;
                } else {
                    std::cerr << "Failed to create scaled surface" << std::endl;
                }
            }
        } else {
            std::cerr << "Image not found: " << config.image_path << std::endl;
        }

        // Initialize font for progress/daemon modes
        if (config.mode != DisplayMode::Simple) {
            if (TTF_Init() < 0) {
                std::cerr << "TTF_Init failed: " << TTF_GetError() << std::endl;
            } else {
                SDL_RWops* rw = SDL_RWFromConstMem(RoundedMplus1c_Bold_reduced_ttf,
                                                    RoundedMplus1c_Bold_reduced_ttf_len);
                if (rw) {
                    font = TTF_OpenFontRW(rw, 1, FONT_SIZE);
                    if (!font) {
                        std::cerr << "Failed to load embedded font: " << TTF_GetError() << std::endl;
                    }
                } else {
                    std::cerr << "Failed to create RWops for embedded font" << std::endl;
                }
            }
        }

        return true;
    }

    void run() {
        start_time = SDL_GetTicks();
        
        switch (config.mode) {
            case DisplayMode::Simple:
            case DisplayMode::Progress:
                runSimpleLoop();
                break;

            case DisplayMode::Daemon:
                runDaemonMode();
                break;
        }
    }

    void stop() {
        running = false;
    }

    bool isRunning() const {
        return running;
    }

    static uint32_t parseColor(const std::string& color_str) {
        std::string hex = color_str;
        if (hex.find("0x") == 0 || hex.find("0X") == 0) {
            hex = hex.substr(2);
        } else if (hex[0] == '#') {
            hex = hex.substr(1);
        }
        return static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
    }

private:
    void runSimpleLoop() {
        while (running) {
            render();
            
            // Check timeout if configured
            if (config.timeout_seconds > 0) {
                uint32_t elapsed_ms = SDL_GetTicks() - start_time;
                uint32_t timeout_ms = static_cast<uint32_t>(config.timeout_seconds) * 1000;
                if (elapsed_ms >= timeout_ms) {
                    running = false;
                    break;
                }
            }
            
            SDL_Delay(1000 / FPS);
        }
    }

    void runDaemonMode() {
        unlink(FIFO_PATH);
        if (mkfifo(FIFO_PATH, 0666) < 0) {
            perror("mkfifo");
        }

        pthread_create(&fifo_thread_handle, nullptr, fifoThreadEntry, this);

        while (running) {
            render();
            SDL_Delay(1000 / FPS);
        }

        pthread_join(fifo_thread_handle, nullptr);
        unlink(FIFO_PATH);
    }

    static void* fifoThreadEntry(void* arg) {
        static_cast<ShowApp*>(arg)->fifoThread();
        return nullptr;
    }

    void fifoThread() {
        char buffer[512];

        while (running) {
            int fd = open(FIFO_PATH, O_RDONLY);
            if (fd < 0) {
                sleep(1);
                continue;
            }

            ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
            if (bytes > 0) {
                buffer[bytes] = '\0';

                // Remove trailing newline
                if (buffer[bytes - 1] == '\n') {
                    buffer[bytes - 1] = '\0';
                }

                // Parse command
                std::string cmd(buffer);
                if (cmd.find("TEXT:") == 0) {
                    pthread_mutex_lock(&mutex);
                    current_text = cmd.substr(5);
                    pthread_mutex_unlock(&mutex);
                } else if (cmd.find("PROGRESS:") == 0) {
                    pthread_mutex_lock(&mutex);
                    current_progress = std::stoi(cmd.substr(9));
                    pthread_mutex_unlock(&mutex);
                } else if (cmd == "QUIT") {
                    running = false;
                } else if (cmd.find("BGCOLOR:") == 0) {
                    pthread_mutex_lock(&mutex);
                    uint32_t rgb = parseColor(cmd.substr(8));
                    bg_color_sdl = SDL_MapRGB(screen->format,
                                              (rgb >> 16) & 0xFF,
                                              (rgb >> 8) & 0xFF,
                                              rgb & 0xFF);
                    pthread_mutex_unlock(&mutex);
                } else if (cmd.find("FONTCOLOR:") == 0) {
                    pthread_mutex_lock(&mutex);
                    uint32_t rgb = parseColor(cmd.substr(10));
                    font_color.r = (rgb >> 16) & 0xFF;
                    font_color.g = (rgb >> 8) & 0xFF;
                    font_color.b = rgb & 0xFF;
                    font_color.a = 255;
                    pthread_mutex_unlock(&mutex);
                } else if (cmd.find("TEXTY:") == 0) {
                    pthread_mutex_lock(&mutex);
                    current_text_y_pct = std::stoi(cmd.substr(6));
                    pthread_mutex_unlock(&mutex);
                } else if (cmd.find("PROGRESSY:") == 0) {
                    pthread_mutex_lock(&mutex);
                    current_progress_y_pct = std::stoi(cmd.substr(10));
                    pthread_mutex_unlock(&mutex);
                }
            }

            close(fd);
        }
    }

    void render() {
        pthread_mutex_lock(&mutex);

        SDL_FillRect(screen, nullptr, bg_color_sdl);

        if (config.mode == DisplayMode::Simple) {
            renderSimple();
        } else {
            renderProgress();
        }

        SDL_UpdateWindowSurface(window);
        pthread_mutex_unlock(&mutex);
    }

    void renderSimple() {
        if (logo) {
            // Center image without stretching - use original dimensions
            SDL_Rect src = {0, 0, logo->w, logo->h};
            SDL_Rect dst = {
                (screen->w - logo->w) / 2,
                (screen->h - logo->h) / 2,
                logo->w,
                logo->h
            };
            SDL_BlitSurface(logo, &src, screen, &dst);
        }
    }

    void renderProgress() {
        // Draw logo - always centered without stretching
        if (logo) {
            SDL_Rect src = {0, 0, logo->w, logo->h};
            SDL_Rect logo_dst = {
                (screen->w - logo->w) / 2,
                (screen->h - logo->h) / 2,
                logo->w,
                logo->h
            };
            SDL_BlitSurface(logo, &src, screen, &logo_dst);
        }

        // Draw text at percentage-based Y position
        if (font && !current_text.empty()) {
            SDL_Surface* text_surface = TTF_RenderUTF8_Blended(font, current_text.c_str(), font_color);
            if (text_surface) {
                int text_y = (screen->h * current_text_y_pct) / 100;
                SDL_Rect text_dst = {
                    (screen->w - text_surface->w) / 2,
                    text_y,
                    text_surface->w,
                    text_surface->h
                };
                SDL_BlitSurface(text_surface, nullptr, screen, &text_dst);
                SDL_FreeSurface(text_surface);
            }
        }

        // Draw progress bar at percentage-based Y position
        int progress_y = (screen->h * current_progress_y_pct) / 100;
        drawProgressBar(progress_y);
    }

    void drawFilledCircle(int cx, int cy, int radius, uint32_t color) {
        for (int y = -radius; y <= radius; y++) {
            for (int x = -radius; x <= radius; x++) {
                if (x * x + y * y <= radius * radius) {
                    int px = cx + x;
                    int py = cy + y;
                    if (px >= 0 && px < screen->w && py >= 0 && py < screen->h) {
                        uint32_t* pixel = (uint32_t*)((uint8_t*)screen->pixels + py * screen->pitch + px * screen->format->BytesPerPixel);
                        *pixel = color;
                    }
                }
            }
        }
    }

    void drawRoundedRect(int x, int y, int w, int h, int radius, uint32_t color) {
        // Draw four corner circles
        drawFilledCircle(x + radius, y + radius, radius, color);
        drawFilledCircle(x + w - radius - 1, y + radius, radius, color);
        drawFilledCircle(x + radius, y + h - radius - 1, radius, color);
        drawFilledCircle(x + w - radius - 1, y + h - radius - 1, radius, color);
        
        // Fill rectangles between corners
        SDL_Rect rects[3] = {
            {x + radius, y, w - 2 * radius, h},           // Center vertical strip
            {x, y + radius, radius, h - 2 * radius},      // Left strip
            {x + w - radius, y + radius, radius, h - 2 * radius}  // Right strip
        };
        for (const auto& rect : rects) {
            SDL_FillRect(screen, &rect, color);
        }
    }

    void drawProgressBar(int y) {
        int progress = current_progress;
        int x = (screen->w - PROGRESS_BAR_WIDTH) / 2;
        int radius = PROGRESS_BAR_HEIGHT / 2;

        // Background with rounded corners
        uint32_t bg_color = SDL_MapRGB(screen->format, 40, 40, 40);
        drawRoundedRect(x, y, PROGRESS_BAR_WIDTH, PROGRESS_BAR_HEIGHT, radius, bg_color);

        uint32_t fill_color = SDL_MapRGB(screen->format, font_color.r, font_color.g, font_color.b);
        
        // Indeterminate progress animation (progress == -1)
        if (progress == -1) {
            // Update animation position
            const float speed = 0.02f;  // Speed of animation
            const float segment_width = 0.3f;  // Width of moving segment (30% of bar)
            
            if (indeterminate_forward) {
                indeterminate_pos += speed;
                if (indeterminate_pos >= 1.0f) {
                    indeterminate_pos = 1.0f;
                    indeterminate_forward = false;
                }
            } else {
                indeterminate_pos -= speed;
                if (indeterminate_pos <= 0.0f) {
                    indeterminate_pos = 0.0f;
                    indeterminate_forward = true;
                }
            }
            
            // Calculate segment position
            int segment_pixel_width = (int)(PROGRESS_BAR_WIDTH * segment_width);
            int max_offset = PROGRESS_BAR_WIDTH - segment_pixel_width;
            int segment_x = x + (int)(max_offset * indeterminate_pos);
            
            // Draw the animated segment
            if (segment_pixel_width > PROGRESS_BAR_HEIGHT) {
                drawRoundedRect(segment_x, y, segment_pixel_width, PROGRESS_BAR_HEIGHT, radius, fill_color);
            } else {
                int circle_radius = PROGRESS_BAR_HEIGHT / 2;
                drawFilledCircle(segment_x + circle_radius, y + circle_radius, circle_radius, fill_color);
            }
        }
        // Normal progress bar (0-100)
        else {
            if (progress < 0) progress = 0;
            if (progress > 100) progress = 100;
            
            if (progress > 0) {
                int fill_width = (PROGRESS_BAR_WIDTH * progress) / 100;
                if (fill_width > PROGRESS_BAR_HEIGHT) {
                    drawRoundedRect(x, y, fill_width, PROGRESS_BAR_HEIGHT, radius, fill_color);
                } else {
                    int circle_radius = PROGRESS_BAR_HEIGHT / 2;
                    drawFilledCircle(x + circle_radius, y + circle_radius, circle_radius, fill_color);
                }
            }
        }
    }

    void cleanup() {
        if (scaled_logo) {
            SDL_FreeSurface(scaled_logo);
            scaled_logo = nullptr;
        }
        
        if (logo) {
            SDL_FreeSurface(logo);
            logo = nullptr;
        }

        if (font) {
            TTF_CloseFont(font);
            TTF_Quit();
            font = nullptr;
        }

        if (window) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }

        SDL_Quit();
    }
};

// Global app pointer for signal handler
static ShowApp* g_app = nullptr;

void signalHandler(int signum) {
    if (signum == SIGINT && g_app) {
        g_app->stop();
    }
}

std::map<std::string, std::string> parseArguments(int argc, char* argv[]) {
    std::map<std::string, std::string> args;

    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        
        if (arg.find("--") == 0) {
            size_t eq_pos = arg.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = arg.substr(2, eq_pos - 2);
                std::string value = arg.substr(eq_pos + 1);
                args[key] = value;
            } else {
                args[arg.substr(2)] = "true";
            }
        }
    }

    return args;
}

void printUsage() {
    std::cout << "Usage:\n";
    std::cout << "  Simple mode:   show2.elf --mode=simple --image=<path> [--bgcolor=0x000000] [--logoheight=N] [--timeout=N]\n";
    std::cout << "  Progress mode: show2.elf --mode=progress --image=<path> [--bgcolor=0x000000] [--fontcolor=0xFFFFFF]\n";
    std::cout << "                 [--text=\"message\"] [--progress=0] [--texty=80] [--progressy=90]\n";
    std::cout << "                 [--logoheight=N] [--timeout=N]\n";
    std::cout << "  Daemon mode:   show2.elf --mode=daemon --image=<path> [--bgcolor=0x000000] [--fontcolor=0xFFFFFF]\n";
    std::cout << "                 [--text=\"message\"] [--texty=80] [--progressy=90] [--logoheight=N]\n";
    std::cout << "\n";
    std::cout << "Position parameters (texty, progressy) are percentages of screen height (0-100)\n";
    std::cout << "Default positions: texty=80, progressy=90\n";
    std::cout << "Logo height parameter (logoheight) scales the logo to the specified height in pixels (0 = no scaling)\n";
    std::cout << "Timeout parameter (timeout) is in seconds (0 = no timeout, runs until killed)\n";
    std::cout << "\n";
    std::cout << "Daemon mode commands via FIFO (" << FIFO_PATH << "):\n";
    std::cout << "  echo \"TEXT:Your message\" > " << FIFO_PATH << "\n";
    std::cout << "  echo \"PROGRESS:50\" > " << FIFO_PATH << "\n";
    std::cout << "  echo \"BGCOLOR:0x123456\" > " << FIFO_PATH << "\n";
    std::cout << "  echo \"FONTCOLOR:0xFFFFFF\" > " << FIFO_PATH << "\n";
    std::cout << "  echo \"TEXTY:80\" > " << FIFO_PATH << "\n";
    std::cout << "  echo \"PROGRESSY:90\" > " << FIFO_PATH << "\n";
    std::cout << "  echo \"QUIT\" > " << FIFO_PATH << "\n";
}

int main(int argc, char* argv[]) {
    auto args = parseArguments(argc, argv);

    if (args.find("help") != args.end() || args.find("image") == args.end() || args.find("mode") == args.end()) {
        printUsage();
        return args.find("help") != args.end() ? 0 : 1;
    }

    Config config;
    config.image_path = args["image"];

    // Parse mode
    std::string mode_str = args["mode"];
    if (mode_str == "simple") {
        config.mode = DisplayMode::Simple;
    } else if (mode_str == "progress") {
        config.mode = DisplayMode::Progress;
    } else if (mode_str == "daemon") {
        config.mode = DisplayMode::Daemon;
    } else {
        std::cerr << "Unknown mode: " << mode_str << std::endl;
        printUsage();
        return 1;
    }

    // Parse optional arguments
    if (args.find("bgcolor") != args.end()) {
        config.bg_color_rgb = ShowApp::parseColor(args["bgcolor"]);
    }

    if (args.find("fontcolor") != args.end()) {
        config.font_color_rgb = ShowApp::parseColor(args["fontcolor"]);
    }

    if (args.find("text") != args.end()) {
        config.text = args["text"];
    }

    if (args.find("progress") != args.end()) {
        config.progress = std::stoi(args["progress"]);
    }

    if (args.find("texty") != args.end()) {
        config.text_y_pct = std::stoi(args["texty"]);
    }

    if (args.find("progressy") != args.end()) {
        config.progress_y_pct = std::stoi(args["progressy"]);
    }

    if (args.find("timeout") != args.end()) {
        config.timeout_seconds = std::stoi(args["timeout"]);
    }

    if (args.find("logoheight") != args.end()) {
        config.logo_height = std::stoi(args["logoheight"]);
    }

    // Create and run app
    ShowApp app(config);
    g_app = &app;

    signal(SIGINT, signalHandler);

    if (!app.initialize()) {
        return 1;
    }

    app.run();

    return 0;
}

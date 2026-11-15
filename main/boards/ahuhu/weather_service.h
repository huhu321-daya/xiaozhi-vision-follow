#ifndef _WEATHER_SERVICE_H_
#define _WEATHER_SERVICE_H_

#include <string>
#include <cstdint>
#include <mutex>

class WeatherService {
public:
    struct WeatherInfo {
        bool valid = false;
        int temperature = 0;
        std::string description;
        std::string weather_type;
        std::string wind_direction;
        double wind_speed = 0.0;
        int wind_level = 0;
        double pressure = 0.0;
        int cloud_rate = 0;
        double precipitation = 0.0;
        std::string precipitation_intensity;
        int pm25 = 0;
        int aqi = 0;
        int aqi_rank = 0;  // AQI全国排名
        int humidity = 0;
        uint64_t last_update_ms = 0;
    };

    static WeatherService* GetInstance();

    void Init(const char* api_token, double lat, double lon);
    void StartAutoUpdate(uint32_t interval_ms = 3600000);
    WeatherInfo GetWeather();
    void FetchWeather();
    bool IsTokenValid() const;
    bool IsWeatherValid() const;
    bool IsValid() const;

private:
    WeatherService() = default;
    ~WeatherService() = default;

    std::string api_token_;
    double lat_ = 0.0;
    double lon_ = 0.0;
    uint32_t interval_ms_ = 0;
    bool enabled_ = false;
    bool update_task_started_ = false;
    WeatherInfo weather_;
    std::mutex mutex_;
};

#endif // _WEATHER_SERVICE_H_
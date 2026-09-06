#include "radio_stations.h"

static const radio_station_t s_stations[] = {
    {
        .name = "动感101",
        .region = "上海",
        .url = "https://lhttp.qtfm.cn/live/274/64k.mp3",
    },
    {
        .name = "Love Radio",
        .region = "上海",
        .url = "https://lhttp.qtfm.cn/live/273/64k.mp3",
    },
    {
        .name = "经典947",
        .region = "上海",
        .url = "https://lhttp.qtfm.cn/live/267/64k.mp3",
    },
    {
        .name = "交通广播",
        .region = "上海",
        .url = "https://lhttp.qtfm.cn/live/266/64k.mp3",
    },
    {
        .name = "新闻广播",
        .region = "上海",
        .url = "https://lhttp.qtfm.cn/live/270/64k.mp3",
    },
};

size_t radio_station_count(void) {
    return sizeof(s_stations) / sizeof(s_stations[0]);
}

const radio_station_t *radio_station_get(size_t index) {
    return index < radio_station_count() ? &s_stations[index] : NULL;
}

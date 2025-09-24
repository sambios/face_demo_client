#pragma once
#include <QtCore>

#include "stream_decode_hw.h"
#include "video_widget.h"

struct TestFaceInfo {
    int64_t pkt_pts;
    int64_t pkt_pos;
    otl::Detection datum;
};

class StreamController:public otl::StreamDecoderEvents
{
    otl::StreamDecoder m_rtsp_reader;
    video_widget *m_video_widget;
    std::string m_rtsp_url;

    std::list<TestFaceInfo> m_faceList;
    std::list<AVFrame*> m_frameList;
    //int64_t m_frame_buffered_num { 0 };
    std::mutex m_framelist_sync;

public:
    StreamController(video_widget *pWidget, const std::string& strUrl, int channel);
    ~StreamController();
    int get_state();
    int start_stream(const std::string& strmFmt, const std::string& pixelFmt, int w, int h);
    int stop_stream();
    void set_frame_bufferd_num(int num);

protected:
    virtual void onDecodedAVFrame(const AVPacket *pkt, const AVFrame *pFrame) override;
    virtual void onDecodedSeiInfo(const uint8_t *sei_data, int sei_data_len, uint64_t pts, int64_t pos) override;


private:
    void video_play_thread_proc();
    std::thread *m_play_thread {nullptr};
    bool m_is_play_thread_running {false};
    int64_t m_last_system_ts {0};
    int64_t m_last_pkt_pts{0};

    // Playback timing
    AVRational m_timebase {1, 1000};
    bool m_timebase_inited {false};
    int64_t m_clock_start_sys_us {0};
    int64_t m_clock_start_pts_us {AV_NOPTS_VALUE};
    // buffering and dropping thresholds
    size_t m_min_buffer_frames {2};
    size_t m_max_queue_frames {30};


};


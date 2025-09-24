#include "stream_controller.h"
#include <qwidget.h>

#include "otl_baseclass.h"
#include "otl_string.h"

StreamController::StreamController(video_widget *pWidget, const std::string& strUrl, int channel):m_video_widget(pWidget),
m_rtsp_url(strUrl), m_rtsp_reader(channel)
{
    m_video_widget->SetTitle(QString("Channel-%1 | 1920x1080").arg(channel));
    m_rtsp_reader.setObserver(this);
}


StreamController::~StreamController()
{
}

int StreamController::get_state()
{
    return 0;
}

int StreamController::start_stream(const std::string& strmFmt, const std::string& pixelFmt, int w, int h)
{
    if (m_play_thread) {
        m_is_play_thread_running = false;
        m_play_thread->join();
        delete m_play_thread;
        m_play_thread = nullptr;
    }

    AVDictionary *opts=NULL;
    av_dict_set(&opts, "pcie_no_copyback", "0", 0);
    //m_rtsp_reader.demuxer().set_param(strmFmt, pixelFmt, w, h);
    int ret = m_rtsp_reader.openStream(m_rtsp_url, true, opts);
    if (ret < 0){
        return ret;
    }

    av_dict_free(&opts);

    // initialize playback timing
    m_timebase = m_rtsp_reader.getTimeBase();
    m_timebase_inited = (m_timebase.num != 0 && m_timebase.den != 0);
    m_clock_start_sys_us = 0;
    m_clock_start_pts_us = AV_NOPTS_VALUE;
    m_last_system_ts = 0;
    m_last_pkt_pts = 0;

    m_is_play_thread_running = true;
    m_play_thread = new std::thread(&StreamController::video_play_thread_proc, this);
    return 0;
}

int StreamController::stop_stream()
{
    if (m_play_thread) {
        m_is_play_thread_running = false;
        m_play_thread->join();
        delete m_play_thread;
        m_play_thread = nullptr;
    }
    m_rtsp_reader.closeStream(false);
    return 0;
}

void StreamController::set_frame_bufferd_num(int num)
{
    //m_frame_buffered_num = num;
}

void StreamController::onDecodedAVFrame(const AVPacket *pkt, const AVFrame *pFrame)
{
#if 0
    static int64_t last_frame_time = 0;
    if (last_frame_time == 0) {
        last_frame_time = av_gettime();
    }else{
        std::cout << "Frame Interval: " << (av_gettime() - last_frame_time)/1000 << std::endl;
        last_frame_time = av_gettime();
    }
#endif
    // Ensure timebase is available (decoder set after async open)
    if (!m_timebase_inited) {
        auto tb = m_rtsp_reader.getTimeBase();
        if (tb.num != 0 && tb.den != 0) {
            m_timebase = tb;
            m_timebase_inited = true;
        }
    }

    auto new_frame = av_frame_clone(pFrame);
    std::lock_guard<std::mutex> lck(m_framelist_sync);
    m_frameList.push_back(new_frame);
    // Prevent unbounded growth to keep latency in check
    while (m_frameList.size() > m_max_queue_frames) {
        auto drop = m_frameList.front();
        m_frameList.pop_front();
        av_frame_unref(drop);
        av_frame_free(&drop);
    }
}




void StreamController::onDecodedSeiInfo(const uint8_t *sei_data, int sei_data_len, uint64_t pkt_pts, int64_t pkt_pos)
{
    TestFaceInfo faceinfo;
    faceinfo.pkt_pts = pkt_pts;
    faceinfo.pkt_pos = pkt_pos;
    //std::string SEI((char*)sei_data, sei_data_len);
    //std::cout << SEI << std::endl;
    std::string sei_raw = otl::base64Dec(sei_data, sei_data_len);

    otl::ByteBuffer buf((const char*)sei_raw.data(), sei_raw.size());
    faceinfo.datum.fromByteBuffer(&buf);

    std::cout << faceinfo.datum.toString() << std::endl;

    std::lock_guard<std::mutex> lck(m_framelist_sync);
    m_faceList.push_back(faceinfo);

}

void StreamController::video_play_thread_proc() {

    const int late_drop_threshold_ms = 80;     // drop if later than this
    const int sleep_quantum_ms = 3;            // sleep granularity

    while (m_is_play_thread_running) {
        if (!m_video_widget) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        AVFrame* frame_to_render = nullptr;
        int64_t frame_pts = AV_NOPTS_VALUE;
        int64_t frame_pts_us = 0;

        {
            std::lock_guard<std::mutex> lck(m_framelist_sync);
            if (m_frameList.empty()) {
                frame_to_render = nullptr;
            } else {
                // buffer a couple frames to smooth jitter
                if (m_frameList.size() < m_min_buffer_frames && m_clock_start_sys_us == 0) {
                    frame_to_render = nullptr;
                } else {
                    AVFrame* f = m_frameList.front();
                    int64_t best_pts = (f->best_effort_timestamp != AV_NOPTS_VALUE) ? f->best_effort_timestamp : f->pkt_pts;
                    frame_pts = best_pts;
                    if (m_timebase_inited && frame_pts != AV_NOPTS_VALUE) {
                        frame_pts_us = av_rescale_q(frame_pts, m_timebase, AVRational{1, 1000000});
                    } else {
                        // fall back: play ASAP
                        frame_pts_us = 0;
                    }

                    // init playback clock at the first presentable frame
                    if (m_clock_start_sys_us == 0 && frame_pts_us != 0) {
                        m_clock_start_sys_us = av_gettime_relative();
                        m_clock_start_pts_us = frame_pts_us;
                    }

                    int64_t now = av_gettime_relative();
                    int64_t due = (m_clock_start_sys_us && frame_pts_us != 0)
                                  ? (m_clock_start_sys_us + (frame_pts_us - m_clock_start_pts_us))
                                  : now;

                    if (due > now) {
                        // not yet time, wait a bit
                        frame_to_render = nullptr;
                    } else {
                        // ready or late: if very late, drop frames to catch up
                        int64_t late_ms = (now - due) / 1000;
                        if (late_ms > late_drop_threshold_ms && m_frameList.size() > 1) {
                            // drop until next frame is closer to due time or queue small
                            while (m_frameList.size() > 1) {
                                AVFrame* drop = m_frameList.front();
                                m_frameList.pop_front();
                                av_frame_unref(drop);
                                av_frame_free(&drop);

                                AVFrame* nf = m_frameList.front();
                                int64_t npts = (nf->best_effort_timestamp != AV_NOPTS_VALUE) ? nf->best_effort_timestamp : nf->pkt_pts;
                                if (npts != AV_NOPTS_VALUE && m_timebase_inited) {
                                    int64_t npts_us = av_rescale_q(npts, m_timebase, AVRational{1, 1000000});
                                    due = m_clock_start_sys_us + (npts_us - m_clock_start_pts_us);
                                    late_ms = (now - due) / 1000;
                                    if (late_ms <= late_drop_threshold_ms) break;
                                } else {
                                    // can't evaluate lateness accurately; stop aggressive dropping
                                    break;
                                }
                            }
                        }

                        // finally pick a frame to render
                        frame_to_render = m_frameList.front();
                        m_frameList.pop_front();
                    }
                }
            }
        }

        if (!frame_to_render) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_quantum_ms));
            continue;
        }

        // Render outside lock
        auto render = m_video_widget->GetVideoHwnd();
        render->draw_frame(frame_to_render);

        // Draw face info whose pts <= current frame pts (or unknown pts)
        {
            std::lock_guard<std::mutex> lck(m_framelist_sync);
            while (!m_faceList.empty()) {
                auto &faceinfo = m_faceList.front();
                if (faceinfo.pkt_pts == AV_NOPTS_VALUE || frame_pts == AV_NOPTS_VALUE || faceinfo.pkt_pts <= frame_pts) {
                    render->draw_info(faceinfo.datum);
                    m_faceList.pop_front();
                } else {
                    break;
                }
            }
        }

        // free frame
        av_frame_unref(frame_to_render);
        av_frame_free(&frame_to_render);
    }

}

#include <Rcpp.h>
#include "whisper.h"

// third-party utilities
// use your favorite implementations
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <cmath>
#include <fstream>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

// Terminal color map. 10 colors grouped in ranges [0.0, 0.1, ..., 0.9]
// Lowest is red, middle is yellow, highest is green.
const std::vector<std::string> k_colors = {
    "\033[38;5;196m", "\033[38;5;202m", "\033[38;5;208m", "\033[38;5;214m", "\033[38;5;220m",
                                    "\033[38;5;226m", "\033[38;5;190m", "\033[38;5;154m", "\033[38;5;118m", "\033[38;5;82m",
};

//  500 -> 00:05.000
// 6000 -> 01:00.000
std::string to_timestamp(int64_t t, bool comma = false) {
    int64_t msec = t * 10;
    int64_t hr = msec / (1000 * 60 * 60);
    msec = msec - hr * (1000 * 60 * 60);
    int64_t min = msec / (1000 * 60);
    msec = msec - min * (1000 * 60);
    int64_t sec = msec / 1000;
    msec = msec - sec * 1000;
    
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d%s%03d", (int) hr, (int) min, (int) sec, comma ? "," : ".", (int) msec);
    
    return std::string(buf);
}

int timestamp_to_sample(int64_t t, int n_samples) {
    return std::max(0, std::min((int) n_samples - 1, (int) ((t*WHISPER_SAMPLE_RATE)/100)));
}

// helper function to replace substrings
void replace_all(std::string & s, const std::string & search, const std::string & replace) {
    for (size_t pos = 0; ; pos += replace.length()) {
        pos = s.find(search, pos);
        if (pos == std::string::npos) break;
        s.erase(pos, search.length());
        s.insert(pos, replace);
    }
}

// command-line parameters
struct whisper_params {
    int32_t n_threads    = std::min(4, (int32_t) std::thread::hardware_concurrency());
    int32_t n_processors = 1;
    int32_t offset_t_ms  = 0;
    int32_t offset_n     = 0;
    int32_t duration_ms  = 0;
    int32_t max_context  = -1;
    int32_t max_len      = 0;
    
    float word_thold = 0.01f;
    
    bool speed_up      = false;
    bool translate     = false;
    bool diarize       = false;
    bool output_txt    = false;
    bool output_vtt    = false;
    bool output_srt    = false;
    bool output_wts    = false;
    bool print_special = false;
    bool print_colors  = false;
    bool no_timestamps = false;
    
    std::string language  = "en";
    std::string model     = "models/ggml-base.en.bin";
    
    std::vector<std::string> fname_inp = {};
};


struct whisper_print_user_data {
    const whisper_params * params;
    
    const std::vector<std::vector<float>> * pcmf32s;
};

void whisper_print_segment_callback(struct whisper_context * ctx, int n_new, void * user_data) {
    const auto & params  = *((whisper_print_user_data *) user_data)->params;
    const auto & pcmf32s = *((whisper_print_user_data *) user_data)->pcmf32s;
    
    const int n_segments = whisper_full_n_segments(ctx);
    
    // print the last n_new segments
    const int s0 = n_segments - n_new;
    if (s0 == 0) {
        Rprintf("\n");
    }
    
    for (int i = s0; i < n_segments; i++) {
        if (params.no_timestamps) {
            if (params.print_colors) {
                for (int j = 0; j < whisper_full_n_tokens(ctx, i); ++j) {
                    if (params.print_special == false) {
                        const whisper_token id = whisper_full_get_token_id(ctx, i, j);
                        if (id >= whisper_token_eot(ctx)) {
                            continue;
                        }
                    }
                    
                    const char * text = whisper_full_get_token_text(ctx, i, j);
                    const float  p    = whisper_full_get_token_p   (ctx, i, j);
                    
                    const int col = std::max(0, std::min((int) k_colors.size(), (int) (std::pow(p, 3)*float(k_colors.size()))));
                    
                    Rprintf("%s%s%s", k_colors[col].c_str(), text, "\033[0m");
                }
            } else {
                const char * text = whisper_full_get_segment_text(ctx, i);
                Rprintf("%s", text);
            }
            Rcpp::checkUserInterrupt();
            //fflush(stdout);
        } else {
            const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
            const int64_t t1 = whisper_full_get_segment_t1(ctx, i);
            
            std::string speaker = "";
            
            if (params.diarize && pcmf32s.size() == 2) {
                const int64_t n_samples = pcmf32s[0].size();
                
                const int64_t is0 = timestamp_to_sample(t0, n_samples);
                const int64_t is1 = timestamp_to_sample(t1, n_samples);
                
                double energy0 = 0.0f;
                double energy1 = 0.0f;
                
                for (int64_t j = is0; j < is1; j++) {
                    energy0 += fabs(pcmf32s[0][j]);
                    energy1 += fabs(pcmf32s[1][j]);
                }
                
                if (energy0 > 1.1*energy1) {
                    speaker = "(speaker 0)";
                } else if (energy1 > 1.1*energy0) {
                    speaker = "(speaker 1)";
                } else {
                    speaker = "(speaker ?)";
                }
                
                //Rprintf("is0 = %lld, is1 = %lld, energy0 = %f, energy1 = %f, %s\n", is0, is1, energy0, energy1, speaker.c_str());
            }
            
            if (params.print_colors) {
                Rprintf("[%s --> %s]  ", to_timestamp(t0).c_str(), to_timestamp(t1).c_str());
                for (int j = 0; j < whisper_full_n_tokens(ctx, i); ++j) {
                    if (params.print_special == false) {
                        const whisper_token id = whisper_full_get_token_id(ctx, i, j);
                        if (id >= whisper_token_eot(ctx)) {
                            continue;
                        }
                    }
                    
                    const char * text = whisper_full_get_token_text(ctx, i, j);
                    const float  p    = whisper_full_get_token_p   (ctx, i, j);
                    
                    const int col = std::max(0, std::min((int) k_colors.size(), (int) (std::pow(p, 3)*float(k_colors.size()))));
                    
                    Rprintf("%s%s%s%s", speaker.c_str(), k_colors[col].c_str(), text, "\033[0m");
                }
                Rprintf("\n");
            } else {
                const char * text = whisper_full_get_segment_text(ctx, i);
                
                Rprintf("[%s --> %s]  %s%s\n", to_timestamp(t0).c_str(), to_timestamp(t1).c_str(), speaker.c_str(), text);
            }
        }
    }
}


// Functionality to free the Rcpp::XPtr
class WhisperModel {
    public: 
        struct whisper_context * ctx;
        WhisperModel(std::string model){
          ctx = whisper_init(model.c_str());
        }
        ~WhisperModel(){
            whisper_free(ctx);
        }
};

// [[Rcpp::export]]
SEXP whisper_load_model(std::string model) {
    // Load language model and return the pointer to be used by whisper_encode
    //struct whisper_context * ctx = whisper_init(model.c_str());
    //Rcpp::XPtr<whisper_context> ptr(ctx, false);
    WhisperModel * wp = new WhisperModel(model);
    Rcpp::XPtr<WhisperModel> ptr(wp, false);
    return ptr;
}
    

// [[Rcpp::export]]
Rcpp::List whisper_encode(SEXP model, std::string path, std::string language, 
                          bool token_timestamps = false, bool translate = false, bool print_special = false, int duration = 0, int offset = 0, bool trace = false,
                          int n_threads = 1, int n_processors = 1) {
    whisper_params params;
    params.language = language;
    //params.model = model;
    params.translate = translate;
    params.print_special = print_special;
    params.duration_ms = duration;
    params.offset_t_ms = offset;
    params.fname_inp.push_back(path);
    params.n_threads = n_threads;
    params.n_processors = n_processors;
    
    
    //std::string language  = "en";
    //std::string model     = "models/ggml-base.en.bin";
    if (params.fname_inp.empty()) {
        Rcpp::stop("error: no input files specified");
    }
    
    if (whisper_lang_id(params.language.c_str()) == -1) {
        Rcpp::stop("Unknown language");
    }
    
    // whisper init
    Rcpp::XPtr<WhisperModel> whispermodel(model);
    struct whisper_context * ctx = whispermodel->ctx;
    //Rcpp::XPtr<whisper_context> ctx(model);
    //struct whisper_context * ctx = whisper_init(params.model.c_str());
    for (int f = 0; f < (int) params.fname_inp.size(); ++f) {
        const auto fname_inp = params.fname_inp[f];
        std::vector<float> pcmf32; // mono-channel F32 PCM
        std::vector<std::vector<float>> pcmf32s; // stereo-channel F32 PCM
        // WAV input
        {
            drwav wav;
            std::vector<uint8_t> wav_data; // used for pipe input from stdin
            
            if (drwav_init_file(&wav, fname_inp.c_str(), NULL) == false) {
                Rcpp::stop("Failed to open the file as WAV file: ", fname_inp);
            }
            
            if (wav.channels != 1 && wav.channels != 2) {
                Rcpp::stop("WAV file must be mono or stereo: ", fname_inp);
            }
            
            if (params.diarize && wav.channels != 2 && params.no_timestamps == false) {
                Rcpp::stop("WAV file must be stereo for diarization and timestamps have to be enabled: ", fname_inp);
            }
            
            if (wav.sampleRate != WHISPER_SAMPLE_RATE) {
                Rcpp::stop("WAV file must be 16 kHz: ", fname_inp);
            }
            
            if (wav.bitsPerSample != 16) {
                Rcpp::stop("WAV file must be 16 bit: ", fname_inp);
            }
            
            const uint64_t n = wav_data.empty() ? wav.totalPCMFrameCount : wav_data.size()/(wav.channels*wav.bitsPerSample/8);
            
            std::vector<int16_t> pcm16;
            pcm16.resize(n*wav.channels);
            drwav_read_pcm_frames_s16(&wav, n, pcm16.data());
            drwav_uninit(&wav);
            
            // convert to mono, float
            pcmf32.resize(n);
            if (wav.channels == 1) {
                for (uint64_t i = 0; i < n; i++) {
                    pcmf32[i] = float(pcm16[i])/32768.0f;
                }
            } else {
                for (uint64_t i = 0; i < n; i++) {
                    pcmf32[i] = float(pcm16[2*i] + pcm16[2*i + 1])/65536.0f;
                }
            }
            
            if (params.diarize) {
                // convert to stereo, float
                pcmf32s.resize(2);
                
                pcmf32s[0].resize(n);
                pcmf32s[1].resize(n);
                for (uint64_t i = 0; i < n; i++) {
                    pcmf32s[0][i] = float(pcm16[2*i])/32768.0f;
                    pcmf32s[1][i] = float(pcm16[2*i + 1])/32768.0f;
                }
            }
        }
        
        /*
        // print system information
        {
            fprintf(stderr, "\n");
            fprintf(stderr, "system_info: n_threads = %d / %d | %s\n",
                    params.n_threads*params.n_processors, std::thread::hardware_concurrency(), whisper_print_system_info());
        }
        */
        {
            if (!whisper_is_multilingual(ctx)) {
                if (params.language != "en" || params.translate) {
                    params.language = "en";
                    params.translate = false;
                    Rcpp::warning("WARNING: model is not multilingual, ignoring language and translation options");
                }
            }
            Rcpp::Rcout << "Processing " << fname_inp << " (" << int(pcmf32.size()) << " samples, " << float(pcmf32.size())/WHISPER_SAMPLE_RATE << " sec)" << ", lang = " << params.language << ", translate = " << params.translate << ", timestamps = " << token_timestamps << "\n";
        }
        
        // run the inference
        {
            whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
            
            wparams.print_realtime   = trace;
            wparams.print_progress   = false;
            wparams.print_timestamps = !params.no_timestamps;
            wparams.print_special    = params.print_special;
            wparams.translate        = params.translate;
            wparams.language         = params.language.c_str();
            wparams.n_threads        = params.n_threads;
            wparams.n_max_text_ctx   = params.max_context >= 0 ? params.max_context : wparams.n_max_text_ctx;
            wparams.offset_ms        = params.offset_t_ms;
            wparams.duration_ms      = params.duration_ms;
            
            wparams.token_timestamps = params.output_wts || params.max_len > 0;
            wparams.token_timestamps = token_timestamps;
            wparams.thold_pt         = params.word_thold;
            wparams.max_len          = params.output_wts && params.max_len == 0 ? 60 : params.max_len;
            
            wparams.speed_up         = params.speed_up;
            
            whisper_print_user_data user_data = { &params, &pcmf32s };
            
            // this callback is called on each new segment
            if (!wparams.print_realtime) {
                wparams.new_segment_callback           = whisper_print_segment_callback;
                wparams.new_segment_callback_user_data = &user_data;
            }
            
            // example for abort mechanism
            // in this example, we do not abort the processing, but we could if the flag is set to true
            // the callback is called before every encoder run - if it returns false, the processing is aborted
            {
                static bool is_aborted = false; // NOTE: this should be atomic to avoid data race
                
                wparams.encoder_begin_callback = [](struct whisper_context * ctx, void * user_data) {
                    bool is_aborted = *(bool*)user_data;
                    return !is_aborted;
                };
                wparams.encoder_begin_callback_user_data = &is_aborted;
            }
            
            if (whisper_full_parallel(ctx, wparams, pcmf32.data(), pcmf32.size(), params.n_processors) != 0) {
                Rcpp::stop("failed to process audio");
            }
        }
    }
    
    // Get the data back in R
    const int n_segments = whisper_full_n_segments(ctx);
    std::vector<int> segment_nr;
    Rcpp::StringVector transcriptions(n_segments);
    Rcpp::StringVector transcriptions_from(n_segments);
    Rcpp::StringVector transcriptions_to(n_segments);
    std::vector<int> token_segment_nr;
    std::vector<std::string> token_segment_text;
    std::vector<float> token_segment_probability;
    std::vector<std::string> token_segment_from;
    std::vector<std::string> token_segment_to;
    for (int i = 0; i < n_segments; ++i) {
        segment_nr.push_back(i + 1);
        const char * text = whisper_full_get_segment_text(ctx, i);
        transcriptions[i] = Rcpp::String(text);
        int64_t t0 = whisper_full_get_segment_t0(ctx, i);
        int64_t t1 = whisper_full_get_segment_t1(ctx, i);
        transcriptions_from[i] = Rcpp::String(to_timestamp(t0).c_str());
        transcriptions_to[i] = Rcpp::String(to_timestamp(t1).c_str());
        
        for (int j = 0; j < whisper_full_n_tokens(ctx, i); ++j) {
            if (params.print_special == false) {
                const whisper_token id = whisper_full_get_token_id(ctx, i, j);
                if (id >= whisper_token_eot(ctx)) {
                    continue;
                }
            }
            const char * text = whisper_full_get_token_text(ctx, i, j);
            const float  p    = whisper_full_get_token_p   (ctx, i, j);
            token_segment_nr.push_back(i + 1);
            std::string str(text);
            token_segment_text.push_back(str);
            token_segment_probability.push_back(p);
            if(token_timestamps){
                whisper_token_data token = whisper_full_get_token_data(ctx, i, j);
                t0 = token.t0;
                t1 = token.t1;
                token_segment_from.push_back(Rcpp::String(to_timestamp(t0).c_str()));
                token_segment_to.push_back(to_timestamp(token.t1));
            }
        }
    }
    Rcpp::DataFrame tokens;
    if(token_timestamps){
        tokens = Rcpp::DataFrame::create(
            Rcpp::Named("segment") = token_segment_nr, 
            Rcpp::Named("token") = token_segment_text, 
            Rcpp::Named("token_prob") = token_segment_probability,
            Rcpp::Named("token_from") = token_segment_from,
            Rcpp::Named("token_to") = token_segment_to,
            Rcpp::Named("stringsAsFactors") = false);
    }else{
        tokens = Rcpp::DataFrame::create(
            Rcpp::Named("segment") = token_segment_nr, 
            Rcpp::Named("token") = token_segment_text, 
            Rcpp::Named("token_prob") = token_segment_probability,
            Rcpp::Named("stringsAsFactors") = false);
    }
    
    //whisper_free(ctx);
    Rcpp::List output = Rcpp::List::create(Rcpp::Named("n_segments") = n_segments,
                                           Rcpp::Named("data") = Rcpp::DataFrame::create(
                                               Rcpp::Named("segment") = segment_nr, 
                                               Rcpp::Named("from") = transcriptions_from,
                                               Rcpp::Named("to") = transcriptions_to,
                                               Rcpp::Named("text") = transcriptions, 
                                               Rcpp::Named("stringsAsFactors") = false),
                                           Rcpp::Named("tokens") = tokens,
                                           Rcpp::Named("params") = Rcpp::List::create(
                                               Rcpp::Named("audio") = path,
                                               Rcpp::Named("language") = params.language, 
                                               Rcpp::Named("offset") = offset,
                                               Rcpp::Named("duration") = duration,
                                               Rcpp::Named("translate") = params.translate,
                                               Rcpp::Named("token_timestamps") = token_timestamps,
                                               Rcpp::Named("word_threshold") = params.word_thold));
    return output;
}

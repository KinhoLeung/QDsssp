#include "qdsssp/eq_widget.h"
#include "qdsssp/audio_spectrum.h"
#include "qdsssp/filter_icon.h"
#include <QApplication>
#include <QComboBox>
#include <QCommandLineParser>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTextStream>
#include <QVBoxLayout>
#include <cmath>
#include <algorithm>
#ifdef Q_OS_LINUX
#include <sys/resource.h>
#endif

using namespace qdsssp;
// Keep benchmark workloads deterministic even if someone touches the screen.
// Normal demo/integration use retains all input handling.
class BenchmarkInputBlocker : public QObject {
    bool eventFilter(QObject *, QEvent *event) override {
        switch (event->type()) {
        case QEvent::TouchBegin: case QEvent::TouchUpdate:
        case QEvent::TouchEnd: case QEvent::TouchCancel:
        case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick: case QEvent::MouseMove:
        case QEvent::Wheel: case QEvent::KeyPress: case QEvent::KeyRelease:
        case QEvent::Shortcut: case QEvent::ShortcutOverride:
            event->accept(); return true;
        default: return false;
        }
    }
};
int main(int argc,char **argv) {
    QApplication app(argc,argv);
    QCoreApplication::setApplicationName("qdsssp_demo");
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOption({"screenshot","Save an 800x480 PNG and exit.","path"});
    parser.addOption({"benchmark","Run continuous editing with 8193 spectrum bins (32 bands by default); report warmed-up wall-clock timings. User input is blocked during this test.","updates","0"});
    parser.addOption({"benchmark-bands","Number of EQ bands in the benchmark (1..32).","bands","32"});
    parser.addOption({"warmup","Input updates excluded from benchmark statistics.","updates","60"});
    parser.addOption({"fps","Display frame rate (10..60).","fps","30"});
    parser.addOption({"no-aa","Disable antialiasing on response curves."});
    parser.addOption({"static-eq","Keep EQ parameters still during the benchmark."});
    parser.addOption({"fill","Enable solid spectrum fill."});
    parser.addOption({"pipewire","Capture a PipeWire sink monitor and feed the spectrum."});
    parser.addOption({"pipewire-uac","Capture the computer-to-board USB Audio Class playback monitor."});
    parser.addOption({"pipewire-target","PipeWire sink/node name; empty uses the default sink monitor.","node"});
    parser.addOption({"file-spectrum","Loop the built-in DOGFIGHT PCM audio and use it as the spectrum source."});
    parser.addOption({"pcm-path","PCM path used by --file-spectrum and --benchmark-pcm.","path"});
    parser.addOption({"benchmark-pcm","Benchmark the local PCM reader and FFT worker together with the widget."});
    parser.addOption({"benchmark-no-spectrum","Benchmark EQ rendering without any spectrum producer."});
    parser.addOption({"benchmark-burst","Publish four synthetic spectrum frames per display tick."});
    parser.addOption({"benchmark-same-band","Benchmark repeated identical setBand() updates."});
    parser.addOption({"benchmark-same-bands","Benchmark repeated identical setBands() snapshots."});
    parser.addOption({"benchmark-snapshot-one-change","Benchmark full snapshots changing only one band."});
    parser.process(app);
    app.setStyleSheet("QWidget { background:#111820; color:#e2ecf2; font:13px 'DejaVu Sans'; }"
        "QComboBox,QPushButton { background:#1d2934; border:1px solid #344451; border-radius:5px; padding:4px 10px; min-height:34px; }"
        "QComboBox::drop-down { border:0; width:18px; }"
        "QPushButton:checked { color:#60c5b2; border-color:#60c5b2; }"
        "QComboBox QAbstractItemView { selection-background-color:#344451; }"
        "QComboBox QAbstractItemView::item { min-height:36px; }");
    QWidget window; window.setWindowTitle("QDsssp — touch equalizer"); window.resize(800,480);
    auto *layout=new QVBoxLayout(&window); layout->setContentsMargins(0,0,0,0); layout->setSpacing(0);
    auto *toolbar=new QHBoxLayout; toolbar->setContentsMargins(18,7,18,5); toolbar->setSpacing(8);
    auto *title=new QLabel("<b>QDsssp</b> <span style='color:#8597a6'> / EQUALIZER</span>"); toolbar->addWidget(title);
    toolbar->addStretch();
    auto *bandSelect=new QComboBox; bandSelect->setMinimumWidth(78); bandSelect->addItem("None"); toolbar->addWidget(bandSelect);
    auto *typeSelect=new QComboBox; typeSelect->setMinimumWidth(154); typeSelect->setIconSize({24,24});
    for (int i=0;i<=12;++i) typeSelect->addItem(filterIcon(FilterType(i)),filterName(FilterType(i)),i);
    toolbar->addWidget(typeSelect);
    auto *enabled=new QPushButton("Band on"); enabled->setCheckable(true); toolbar->addWidget(enabled);
    auto *spectrum=new QPushButton("Spectrum"); spectrum->setCheckable(true); spectrum->setChecked(true); toolbar->addWidget(spectrum);
    layout->addLayout(toolbar);
    auto *eq=new EqWidget; layout->addWidget(eq,1);
    eq->setFrameRate(parser.value("fps").toInt());
    eq->setCurveAntialiasing(!parser.isSet("no-aa")); eq->setSpectrumFillEnabled(parser.isSet("fill"));
    const int benchmarkFrames=parser.value("benchmark").toInt();
    const bool benchmarking=benchmarkFrames>0;
    const int parameterModes=int(parser.isSet("benchmark-same-band"))+int(parser.isSet("benchmark-same-bands"))+
                             int(parser.isSet("benchmark-snapshot-one-change"));
    if (parameterModes>1) {
        QTextStream(stderr) << "Choose only one parameter benchmark mode.\n";
        return 2;
    }
    const bool benchmarkPcm=benchmarking && (parser.isSet("benchmark-pcm") || parser.isSet("file-spectrum"));
    const bool benchmarkNoSpectrum=benchmarking && parser.isSet("benchmark-no-spectrum");
    if (benchmarkPcm && benchmarkNoSpectrum) {
        QTextStream(stderr) << "Choose either --benchmark-pcm or --benchmark-no-spectrum, not both.\n";
        return 2;
    }
    BenchmarkInputBlocker inputBlocker;
    if (benchmarkFrames>0) app.installEventFilter(&inputBlocker);
    const int count=benchmarking?parser.value("benchmark-bands").toInt():6;
    if (count<1 || count>ResponseCache::MaxBands) {
        QTextStream(stderr) << "Use 1..32 benchmark bands.\n";
        return 2;
    }
    const int editedBand=std::min(2,count-1);
    std::vector<Band> bands(count);
    for (int i=0;i<count;++i) {
        bands[i].frequency=50*std::pow(240.0,double(i)/std::max(1,count-1));
        bands[i].gainDb=std::sin(i*1.7)*5; bands[i].q=1.2+0.3*i;
        bandSelect->addItem(QString("EQ %1").arg(i+1));
    }
    if (count==6) { bands[0].type=FilterType::LowShelf2; bands[0].gainDb=4; bands[5].type=FilterType::HighShelf2; bands[5].gainDb=2; }
    eq->setBands(bands); eq->setSelectedBand(editedBand);
    auto syncControls=[&] {
        const int i=eq->selectedBand();
        QSignalBlocker a(bandSelect),b(typeSelect),c(enabled);
        bandSelect->setCurrentIndex(i+1);
        typeSelect->setEnabled(i>=0); enabled->setEnabled(i>=0);
        if(i<0) return;
        typeSelect->setCurrentIndex(int(eq->bands()[i].type));
        enabled->setChecked(eq->bands()[i].enabled); enabled->setText(eq->bands()[i].enabled?"Band on":"Band off");
    };
    QObject::connect(eq,&EqWidget::selectedBandChanged,&window,syncControls);
    QObject::connect(bandSelect,QOverload<int>::of(&QComboBox::currentIndexChanged),&window,[&](int index) { eq->setSelectedBand(index-1); });
    QObject::connect(typeSelect,QOverload<int>::of(&QComboBox::currentIndexChanged),&window,[&](int type) {
        const int i=eq->selectedBand(); if(i<0) return; auto b=eq->bands()[i]; b.type=FilterType(type); eq->setBand(i,b);
    });
    QObject::connect(enabled,&QPushButton::toggled,&window,[&](bool on) {
        const int i=eq->selectedBand(); if(i<0) return; auto b=eq->bands()[i]; b.enabled=on; eq->setBand(i,b); syncControls();
    });
    QObject::connect(spectrum,&QPushButton::toggled,eq,&EqWidget::setSpectrumEnabled);
    syncControls();

    std::unique_ptr<PipeWireSpectrumSource> audioSource;
    std::unique_ptr<PcmFileSpectrumSource> fileSource;
    if (parser.isSet("pipewire") && (parser.isSet("file-spectrum") || parser.isSet("benchmark-pcm"))) {
        QTextStream(stderr) << "Choose either --pipewire or --file-spectrum, not both.\n";
        return 2;
    }
    if (parser.isSet("pipewire") && !benchmarkNoSpectrum) {
        PipeWireSpectrumSource::Config config;
        QString targetNode = parser.value("pipewire-target");
        if (parser.isSet("pipewire-uac")) {
            targetNode = qEnvironmentVariable("QDSSSP_PIPEWIRE_UAC_NODE");
            if (targetNode.isEmpty()) {
                targetNode = QStringLiteral(
                    "alsa_output._sys_devices_platform_ff740000.usb_gadget.0_sound_card1.stereo-fallback");
            }
        }
        config.targetNode = targetNode.toStdString();
        audioSource.reset(new PipeWireSpectrumSource(eq->spectrumInput()));
        if (!audioSource->start(config)) {
            QTextStream(stderr) << "Could not start PipeWire spectrum source: "
                                << QString::fromStdString(audioSource->error()) << '\n';
            return 2;
        }
    }
    // The board demo defaults to the deterministic DOGFIGHT PCM source so a
    // plain Qt Creator Run starts with a visible spectrum. --pipewire remains
    // the explicit opt-in for live audio input.
    // A normal run uses PCM by default. A benchmark selects its source
    // explicitly so the historical display baseline remains deterministic
    // and does not silently include a worker thread doing FFT work.
    const bool usePcm = parser.isSet("file-spectrum") || parser.isSet("benchmark-pcm") ||
                        (!benchmarking && !parser.isSet("pipewire"));
    if (usePcm && !benchmarkNoSpectrum) {
        PcmFileSpectrumSource::Config config;
        if (parser.isSet("pcm-path")) config.path = parser.value("pcm-path").toStdString();
        fileSource.reset(new PcmFileSpectrumSource(eq->spectrumInput()));
        if (!fileSource->start(config)) {
            QTextStream(stderr) << "Could not start PCM file spectrum source: "
                                << QString::fromStdString(fileSource->error()) << '\n';
            return 2;
        }
    }

    // Precompute spatial terms. The per-frame sine addition is equivalent to
    // the original demo waveform without thousands of GUI-thread libm calls.
    auto mailbox=eq->spectrumInput();
    std::vector<float> bins(benchmarking?8193:2049,-100);
    struct SpectrumTerms { double base, sinA, cosA, sinB, cosB; };
    std::vector<SpectrumTerms> terms(bins.size());
    for (int k=0;k<int(bins.size());++k) {
        const double hz=24000.0*k/(bins.size()-1), x=std::log(std::max(20.0,hz)/20.0);
        terms[k]={-75+20*std::sin(x*0.6),10*std::sin(x*2.8),10*std::cos(x*2.8),
                  5*std::sin(x*13),5*std::cos(x*13)};
    }
    const int fps=std::max(10,std::min(60,parser.value("fps").toInt()));
    const int burst = parser.isSet("benchmark-burst") ? 4 : 1;
    const int warmup=std::max(0,parser.value("warmup").toInt());
    QElapsedTimer elapsed;
    TimingSamples sourceTiming;
    if (benchmarkFrames>0) sourceTiming.samples.reserve(4096);
    unsigned long evaluationStart=0;
#ifdef Q_OS_LINUX
    auto cpuSeconds=[] {
        rusage usage{}; getrusage(RUSAGE_SELF,&usage);
        return usage.ru_utime.tv_sec+usage.ru_utime.tv_usec/1e6+
               usage.ru_stime.tv_sec+usage.ru_stime.tv_usec/1e6;
    };
    double cpuStart=0;
#endif
    QTimer source; source.setTimerType(Qt::PreciseTimer); int frame=0;
    QObject::connect(&source,&QTimer::timeout,&window,[&] {
        if (benchmarkFrames>0 && frame==warmup) {
            eq->startProfiling(); elapsed.start(); evaluationStart=eq->responseEvaluations();
            if (fileSource) fileSource->resetStats();
#ifdef Q_OS_LINUX
            cpuStart=cpuSeconds();
#endif
        }
        ++frame;
        QElapsedTimer sourceClock; sourceClock.start();
        const bool syntheticSpectrum = !audioSource && !fileSource && !benchmarkNoSpectrum && spectrum->isChecked();
        if (syntheticSpectrum) {
            for (int burstIndex=0; burstIndex<burst; ++burstIndex) {
                const int phaseFrame=frame*burst+burstIndex;
                const double ca=std::cos(phaseFrame*0.04),sa=std::sin(phaseFrame*0.04);
                const double cb=std::cos(phaseFrame*0.07),sb=std::sin(phaseFrame*0.07);
                for (int k=0;k<int(bins.size());++k) {
                    const auto &t=terms[k];
                    bins[k]=float(t.base+t.sinA*ca+t.cosA*sa+t.sinB*cb+t.cosB*sb);
                }
                mailbox->publish(bins.data(),int(bins.size()),48000);
            }
        }
        if (benchmarkFrames>0) {
            if (!parser.isSet("static-eq")) {
                if (parser.isSet("benchmark-same-band")) {
                    eq->setBand(editedBand,eq->bands()[editedBand]);
                } else if (parser.isSet("benchmark-same-bands")) {
                    eq->setBands(bands);
                } else if (parser.isSet("benchmark-snapshot-one-change")) {
                    bands[editedBand].frequency=800+500*std::sin(frame*0.04);
                    bands[editedBand].gainDb=12*std::sin(frame*0.07);
                    bands[editedBand].q=0.5+8*(0.5+0.5*std::sin(frame*0.02));
                    eq->setBands(bands);
                } else {
                    auto b=eq->bands()[editedBand]; b.frequency=800+500*std::sin(frame*0.04);
                    b.gainDb=12*std::sin(frame*0.07); b.q=0.5+8*(0.5+0.5*std::sin(frame*0.02)); eq->setBand(editedBand,b);
                }
            }
            if (frame>warmup) sourceTiming.add(sourceClock.nsecsElapsed());
            if (frame>=warmup+benchmarkFrames) {
                source.stop();
                // Allow the last queued update to paint; the drain is included
                // in elapsed time, so short runs slightly understate throughput.
                QTimer::singleShot(100,&window,[&] {
                    if (fileSource) fileSource->stop();
                    if (audioSource) audioSource->stop();
                    eq->stopProfiling();
                    const double seconds=elapsed.nsecsElapsed()/1e9;
                    QTextStream out(stdout); out.setRealNumberNotation(QTextStream::FixedNotation); out.setRealNumberPrecision(3);
                    const auto &profile=*eq->profile();
                    const char *scenario = benchmarkNoSpectrum ? "eq-only" :
                        (fileSource ? (parser.isSet("static-eq") ? "pcm-static" : "pcm") :
                         (burst > 1 ? (parser.isSet("static-eq") ? "synthetic-static-burst" : "synthetic-burst") :
                          (parser.isSet("static-eq") ? "synthetic-static" : "synthetic-dynamic")));
                    const bool syntheticInput = !audioSource && !fileSource && !benchmarkNoSpectrum && spectrum->isChecked();
                    out<<"Qt "<<qVersion()<<", scenario="<<scenario
                       <<", input_source="<<(fileSource ? "pcm" : (syntheticInput ? "synthetic" : "none"))
                       <<", window="<<window.width()<<"x"<<window.height()
                       <<", widget="<<eq->width()<<"x"<<eq->height()<<", bands="<<count<<", bins="<<bins.size()
                       <<", target_fps="<<fps<<", burst="<<burst
                       <<", static_eq="<<parser.isSet("static-eq")
                       <<", parameter_mode="<<(parser.isSet("benchmark-same-band")?"same-band":
                           parser.isSet("benchmark-same-bands")?"same-bands":
                           parser.isSet("benchmark-snapshot-one-change")?"one-change-snapshot":"normal")
                       <<", input_blocked=1\n"
                       <<"warmup_updates="<<warmup<<", measured_updates="<<benchmarkFrames
                       <<", elapsed_s="<<seconds<<", painted="<<profile.phases[FrameProfile::Paint].count
                       <<", paint_fps="<<profile.phases[FrameProfile::Paint].count/seconds
                       <<", response_evaluations="<<eq->responseEvaluations()-evaluationStart<<"\n";
#ifdef Q_OS_LINUX
                    rusage usage{}; getrusage(RUSAGE_SELF,&usage);
                    out<<"process_cpu_percent_one_core="<<100*(cpuSeconds()-cpuStart)/seconds
                       <<", peak_rss_kib="<<usage.ru_maxrss<<"\n";
#endif
                    auto report=[&](const char *name,const TimingSamples &timing) {
                        auto sorted=timing.samples; std::sort(sorted.begin(),sorted.end());
                        auto percentile=[&](double fraction) {
                            return sorted.empty()?0.0:sorted[size_t(std::ceil(fraction*sorted.size()))-1]/1e6;
                        };
                        out<<name<<" count="<<timing.count<<", mean/p50/p95/p99/max_ms="
                           <<timing.totalNs/1e6/std::max<quint64>(1,timing.count)<<"/"
                           <<percentile(0.50)<<"/"<<percentile(0.95)<<"/"<<percentile(0.99)<<"/"<<timing.maxNs/1e6
                           <<", sampled="<<sorted.size()<<"\n";
                    };
                    const char *names[]={"prepare","response","paths","overlay","curves","controls","spectrum","paint","paint_interval","footer"};
                    for (int i=0;i<FrameProfile::PhaseCount;++i) report(names[i],profile.phases[i]);
                    report("demo_source",sourceTiming);
                    if (fileSource) {
                        const auto stats=fileSource->stats();
                        out<<"pcm_input_samples="<<stats.inputSamples
                           <<", pcm_dropped_samples="<<stats.droppedSamples
                           <<", pcm_fft_frames="<<stats.fftFrames
                           <<", pcm_published_frames="<<stats.publishedFrames
                           <<", pcm_fft_mean_ms="<<(stats.fftFrames ? double(stats.fftNanoseconds)/stats.fftFrames/1e6 : 0.0)
                           <<", pcm_fft_max_ms="<<double(stats.fftMaxNanoseconds)/1e6<<"\n";
                    }
                    app.quit();
                });
            }
        }
    });
    source.start(qRound(1000.0/fps));
    window.show();
    if (parser.isSet("screenshot")) QTimer::singleShot(350,&window,[&] {
        const bool saved=window.grab().save(parser.value("screenshot")); app.exit(saved?0:1);
    });
    return app.exec();
}

#include "qdsssp/eq_widget.h"
#include "qdsssp/audio_spectrum.h"
#include "qdsssp/filter_icon.h"
#include "../src/response_path.h"
#include "../src/overlay_region.h"
#include <QPainter>
#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTouchDevice>
#include <QTouchEvent>
#include <atomic>
#include <cmath>
#include <complex>
#include <limits>
#include <thread>

using namespace qdsssp;
class Tests : public QObject {
    Q_OBJECT
private slots:
    void filterInvariants() {
        for (int type=0;type<=12;++type) {
            Band b; b.type=FilterType(type); b.frequency=1000; b.q=std::sqrt(0.5); b.gainDb=9;
            auto c=designBiquad(b,48000);
            const double center=magnitudeDb(c,1000,48000);
            if (b.type==FilterType::Peak || b.type==FilterType::Gain) QVERIFY(std::abs(center-9)<1e-7);
            if (b.type==FilterType::Notch) QVERIFY(center < -150);
            if (b.type==FilterType::BandPass || b.type==FilterType::Bypass) QVERIFY(std::abs(center)<1e-8);
            if (type>=7 && type<=10) QVERIFY(std::abs(center+3.01029995664)<1e-7);
            if (b.type==FilterType::LowShelf1 || b.type==FilterType::LowShelf2) {
                QVERIFY(std::abs(magnitudeDb(c,0,48000)-9)<1e-7);
                QVERIFY(std::abs(magnitudeDb(c,24000,48000))<1e-7);
            }
            if (b.type==FilterType::HighShelf1 || b.type==FilterType::HighShelf2) {
                QVERIFY(std::abs(magnitudeDb(c,0,48000))<1e-7);
                QVERIFY(std::abs(magnitudeDb(c,24000,48000)-9)<1e-7);
            }
            if (hasGain(b.type)) {
                b.gainDb=-9; const auto cut=designBiquad(b,48000);
                for (double hz:{20.0,200.0,1000.0,10000.0,20000.0})
                    QVERIFY(std::abs(magnitudeDb(c,hz,48000)+magnitudeDb(cut,hz,48000))<1e-6);
            }
            b.enabled=false;
            QCOMPARE(magnitudeDb(designBiquad(b,48000),1000,48000),0.0);
        }
    }
    void numericalExtremesAndIndependentResponse() {
        for (double fs:{8000.0,44100.0,48000.0,96000.0})
        for (int type=0;type<=12;++type)
        for (double q:{0.1,0.707,30.0})
        for (double gain:{-24.0,0.0,24.0})
        for (double hz:{20.0,1000.0,fs*0.499}) {
            Band b; b.type=FilterType(type); b.q=q; b.gainDb=gain; b.frequency=hz;
            const auto c=designBiquad(b,fs);
            for (double f:{25.0,1500.0,fs*0.49}) {
                const double db=magnitudeDb(c,f,fs); QVERIFY(std::isfinite(db));
                const auto z=std::polar(1.0,-2*3.14159265358979323846*f/fs);
                const double expected=20*std::log10(std::abs((c.b0+c.b1*z+c.b2*z*z)/(1.0+c.a1*z+c.a2*z*z)));
                if (expected>-180) QVERIFY2(std::abs(expected-db)<1e-5,"Response differs from independent complex transfer function");
            }
        }
        Band invalid; invalid.frequency=std::numeric_limits<double>::infinity();
        invalid.q=std::numeric_limits<double>::quiet_NaN();
        invalid.gainDb=std::numeric_limits<double>::quiet_NaN();
        QVERIFY(std::isfinite(magnitudeDb(designBiquad(invalid,48000),1000,48000)));
    }
    void cacheAndCustomCoefficients() {
        ResponseCache cache; std::vector<Band> bands(32); bands[0].gainDb=10;
        cache.setBands(bands); QVERIFY(cache.update()); QCOMPARE(cache.evaluationCount(),32UL);
        QVERIFY(!cache.update()); QCOMPARE(cache.evaluationCount(),32UL);
        bands[4].gainDb=-3; cache.setBand(4,bands[4]); QVERIFY(cache.update());
        QCOMPARE(cache.evaluationCount(),33UL);
        for (int i=0;i<int(cache.frequencies().size());i+=11) {
            const double hz=cache.frequencies()[i];
            const double expected=magnitudeDb(designBiquad(bands[0],48000),hz,48000)+
                                  magnitudeDb(designBiquad(bands[4],48000),hz,48000);
            QVERIFY(std::abs(cache.sumDb()[i]-expected)<1e-8);
        }
        Band custom; custom.useCustomCoefficients=true; custom.coefficients={2,0,0,0,0};
        cache.setBands({custom}); cache.update();
        QVERIFY(std::abs(cache.sumDb()[0]-6.02059991328)<1e-8);
        cache.setBands({}); cache.update(); QCOMPARE(cache.sumDb()[0],0.0);
    }
    void noOpParameterUpdatesAvoidWork() {
        ResponseCache cache;
        std::vector<Band> bands(32);
        cache.setBands(bands); QVERIFY(cache.update());
        QCOMPARE(cache.evaluationCount(),32UL);

        for (int i=0;i<90;++i) {
            cache.setBand(5,bands[5]);
            QVERIFY(!cache.update());
        }
        QCOMPARE(cache.evaluationCount(),32UL);

        for (int i=0;i<90;++i) {
            cache.setBands(bands);
            QVERIFY(!cache.update());
        }
        QCOMPARE(cache.evaluationCount(),32UL);

        bands[5].gainDb=6;
        cache.setBands(bands); QVERIFY(cache.update());
        QCOMPARE(cache.evaluationCount(),33UL);

        bands[17].q=3;
        cache.setBands(bands); QVERIFY(cache.update());
        QCOMPARE(cache.evaluationCount(),34UL);

        // The widget applies the same no-op guard before scheduling a frame.
        EqWidget widget; widget.resize(800,480); widget.setSpectrumEnabled(false);
        widget.setBands(bands); widget.show(); QTest::qWait(80);
        const auto evaluations=widget.responseEvaluations();
        const auto paints=widget.paintedFrames();
        for (int i=0;i<90;++i) widget.setBand(5,bands[5]);
        for (int i=0;i<90;++i) widget.setBands(bands);
        QTest::qWait(100);
        QCOMPARE(widget.responseEvaluations(),evaluations);
        QCOMPARE(widget.paintedFrames(),paints);
    }
    void incrementalSumStaysAccurate() {
        ResponseCache cache;
        std::vector<Band> bands(32);
        for (int i=0;i<32;++i) { bands[i].frequency=30*std::pow(500.0,i/31.0); bands[i].gainDb=i%13-6; }
        cache.setGrid(257,20,20000,48000); cache.setBands(bands); cache.update();
        auto verify=[&] {
            for (int k=0;k<int(cache.frequencies().size());++k) {
                double expected=0;
                for (const auto &band:cache.bands()) expected+=magnitudeDb(designBiquad(band,48000),cache.frequencies()[k],48000);
                QVERIFY(std::abs(cache.sumDb()[k]-expected)<1e-8);
            }
        };
        for (int edit=0;edit<1100;++edit) {
            const int index=edit%32;
            bands[index].gainDb=24*std::sin(edit*0.17);
            bands[index].q=0.1+29.9*(0.5+0.5*std::sin(edit*0.03));
            bands[index].enabled=edit%7!=0;
            cache.setBand(index,bands[index]);
            // Multiple writes before a frame must subtract the last DISPLAYED
            // response, including an unrelated second dirty band.
            bands[index].gainDb*=0.9; cache.setBand(index,bands[index]);
            cache.setBand((index+1)%32,bands[(index+1)%32]); cache.update();
            if (edit%127==0 || edit==1099) verify();
        }
        cache.setGrid(301,30,16000,48000); cache.update(); verify();
        bands.resize(3); cache.setBands(bands); cache.update(); verify();
        cache.setBands({}); cache.update(); verify();
    }
    void responsePolylineErrorBound() {
        const QRectF rect(48,26,696,290);
        ResponseCache cache; cache.setGrid(1392,20,20000,48000);
        auto verify=[&](const std::vector<double> &db,double dpr) {
            const double tolerance=0.2/dpr;
            const auto path=detail::responsePath(db,rect,tolerance);
            QCOMPARE(path.elementAt(0).x,rect.left());
            QVERIFY(std::abs(path.elementAt(path.elementCount()-1).x-rect.right())<1e-9);
            int segment=1;
            for (int k=0;k<int(db.size());++k) {
                const double x=rect.left()+k*rect.width()/(db.size()-1);
                while (segment+1<path.elementCount() && path.elementAt(segment).x<x) ++segment;
                const auto a=path.elementAt(segment-1),b=path.elementAt(segment);
                const double y=a.y+(b.y-a.y)*(x-a.x)/(b.x-a.x);
                const double expected=rect.center().y()-std::max(-72.0,std::min(72.0,db[k]))*rect.height()/48;
                QVERIFY2(std::abs(y-expected)<=tolerance+1e-8,"Simplified response exceeded its pixel error budget");
            }
        };
        for (int type=0;type<=12;++type) for (double q:{0.1,0.707,30.0}) for (double hz:{25.0,1000.0,19000.0}) {
            Band band; band.type=FilterType(type); band.q=q; band.gainDb=-24; band.frequency=hz;
            cache.setBands({band}); cache.update();
            verify(cache.sumDb(),1); verify(cache.sumDb(),2);
        }
        std::vector<double> adversarial(1392);
        for (int i=0;i<int(adversarial.size());++i) adversarial[i]=i%31==0?100:20*std::sin(i*1.71);
        verify(adversarial,1); verify(adversarial,1.5);
        std::fill(adversarial.begin(),adversarial.end(),0);
        QCOMPARE(detail::responsePath(adversarial,rect,0.2).elementCount(),2);
        QVERIFY(detail::responsePath({},rect,0.2).isEmpty());
    }
    void cachedAppearanceFollowsState() {
        EqWidget widget; widget.resize(800,480); widget.setSpectrumEnabled(false);
        std::vector<Band> bands(4);
        for (int i=0;i<4;++i) { bands[i].frequency=100*std::pow(5,i); bands[i].gainDb=i*3-6; }
        widget.setBands(bands); widget.show(); QTest::qWait(180);
        // A warmed cache must produce the same pixels as a fresh widget after
        // changing selection, enable state, font, size and band count.
        for (int step=0;step<9;++step) {
            if (step==1) { bands[1].enabled=false; widget.setBand(1,bands[1]); }
            if (step==2) { auto font=widget.font(); font.setItalic(true); widget.setFont(font); widget.resize(640,400); }
            if (step==3) { bands.resize(2); widget.setBands(bands); }
            if (step==4) { bands[0].gainDb=24; bands[0].q=30; widget.setBand(0,bands[0]); }
            if (step==5) { bands[1].gainDb=-24; bands[1].q=0.1; bands[1].useCustomCoefficients=true; widget.setBand(1,bands[1]); }
            if (step==6) widget.setEnabled(false);
            if (step==7) { widget.setEnabled(true); bands.clear(); widget.setBands(bands); }
            if (step==8) { bands.resize(3); widget.setBands(bands); }
            widget.setSelectedBand(bands.empty()?-1:step%int(bands.size()));
            EqWidget fresh; fresh.resize(widget.size()); fresh.setFont(widget.font());
            fresh.setSpectrumEnabled(false); fresh.setBands(bands); fresh.setSelectedBand(widget.selectedBand());
            fresh.setEnabled(widget.isEnabled());
            fresh.show(); QTest::qWait(180);
            QCOMPARE(widget.grab().toImage(),fresh.grab().toImage());
        }
    }
    void sparseOverlayPreservesPixels() {
        const QRect bounds(30,8,732,326);
        const QRectF plot(48,26,696,290),clip=plot.adjusted(-1,-1,1,1);
        ResponseCache cache; cache.setGrid(1392,20,20000,48000);
        for (double dpr:{1.0,1.5,2.0}) for (bool aa:{false,true}) {
            for (int type=0;type<=12;++type) {
                Band band; band.type=FilterType(type); band.gainDb=24; band.q=30;
                band.frequency=type%2?20:19000; cache.setBands({band}); cache.update();
                auto path=detail::responsePath(cache.sumDb(),plot,0.2/dpr);
                detail::OverlayCoverage coverage(bounds); coverage.addPath(path,clip,2+2/dpr);
                QImage layer(QSize(qCeil(bounds.width()*dpr),qCeil(bounds.height()*dpr)),QImage::Format_ARGB32_Premultiplied);
                layer.setDevicePixelRatio(dpr); layer.fill(Qt::transparent);
                {
                    QPainter p(&layer); p.translate(-bounds.topLeft()); p.setClipRect(clip);
                    p.setRenderHint(QPainter::Antialiasing,aa); p.setPen(QPen(Qt::yellow,1.5)); p.drawPath(path);
                }
                QImage dense(QSize(qCeil(800*dpr),qCeil(480*dpr)),QImage::Format_RGB32);
                dense.setDevicePixelRatio(dpr); dense.fill(QColor(17,24,32)); QImage sparse=dense;
                { QPainter p(&dense); p.drawImage(bounds.topLeft(),layer); }
                { QPainter p(&sparse); p.setClipRect(coverage.rect()); p.drawImage(bounds.topLeft(),layer); }
                QCOMPARE(sparse,dense);
            }
        }
    }
    void footerPartialUpdates() {
        EqWidget widget; widget.resize(800,480); widget.setSpectrumEnabled(false);
        std::vector<Band> bands(3); widget.setBands(bands); widget.setSelectedBand(1);
        widget.show(); QTest::qWait(180);
        widget.startProfiling();
        bands[0].gainDb=10; widget.setBand(0,bands[0]); QTest::qWait(140);
        QCOMPARE(widget.profile()->phases[FrameProfile::Footer].count,quint64(0));
        for (int step=0;step<7;++step) {
            auto &b=bands[1];
            if (step==0) b.frequency=2500;
            if (step==1) b.gainDb=-12;
            if (step==2) b.q=30;
            if (step==3) b.type=FilterType::LowShelf1;
            if (step==4) b.gainDb=12;
            if (step==5) b.useCustomCoefficients=true;
            if (step==6) b.useCustomCoefficients=false;
            widget.setBand(1,b);
            EqWidget fresh; fresh.resize(widget.size()); fresh.setSpectrumEnabled(false);
            fresh.setBands(bands); fresh.setSelectedBand(1); fresh.show(); QTest::qWait(180);
            QCOMPARE(widget.grab().toImage(),fresh.grab().toImage());
        }
        QVERIFY(widget.profile()->phases[FrameProfile::Footer].count>0);
        widget.stopProfiling();
    }
    void dynamicOverlayUsesPaintThenCaches() {
        EqWidget widget; widget.resize(800,480); widget.setSpectrumEnabled(false);
        std::vector<Band> bands(2); bands[0].frequency=250; bands[0].gainDb=4;
        bands[1].frequency=4000; bands[1].gainDb=-3; widget.setBands(bands);
        widget.setSelectedBand(0); widget.show(); QTest::qWait(180);

        widget.startProfiling();
        bands[0].gainDb=14; widget.setBand(0,bands[0]);
        QTest::qWait(70); // shorter than the three-frame quiet period
        const auto *early=widget.profile();
        QVERIFY(early->phases[FrameProfile::Curves].count>0);
        QVERIFY(early->phases[FrameProfile::Controls].count>0);
        const auto earlyOverlay=early->phases[FrameProfile::Overlay].count;

        QTest::qWait(180); // allow the quiet-period bitmap rebuild
        QVERIFY(widget.profile()->phases[FrameProfile::Overlay].count>earlyOverlay);

        EqWidget fresh; fresh.resize(widget.size()); fresh.setSpectrumEnabled(false);
        fresh.setBands(bands); fresh.setSelectedBand(0); fresh.show(); QTest::qWait(180);
        QCOMPARE(widget.grab().toImage(),fresh.grab().toImage());
        widget.stopProfiling();
    }
    void spectrumValidationAndPeaks() {
        SpectrumMailbox mailbox; SpectrumFrame frame;
        std::vector<float> bins(8193,-100); bins[6000]=-3;
        QVERIFY(!mailbox.publish(nullptr,8193,48000));
        QVERIFY(!mailbox.publish(bins.data(),8194,48000));
        QVERIFY(!mailbox.publish(bins.data(),8193,std::numeric_limits<double>::quiet_NaN()));
        QVERIFY(mailbox.publish(bins.data(),8193,48000)); bins[6000]=-7;
        QVERIFY(mailbox.publish(bins.data(),8193,48000)); QVERIFY(mailbox.takeLatest(frame));
        QCOMPARE(frame.sequence,std::uint64_t(2)); QVERIFY(!mailbox.takeLatest(frame));
        SpectrumProjector projector; projector.configure(384,20,20000,8193,48000);
        std::vector<float> result; projector.project(frame,result);
        QCOMPARE(result.size(),size_t(384)); QCOMPARE(*std::max_element(result.begin(),result.end()),-7.0f);
        // Every visible high-frequency FFT bin must survive peak pooling.
        frame.db.fill(-100);
        for (int bin=100;bin<6826;bin+=37) {
            frame.db[bin]=-1; projector.project(frame,result);
            QCOMPARE(*std::max_element(result.begin(),result.end()),-1.0f); frame.db[bin]=-100;
        }
        frame.db[500]=std::numeric_limits<float>::quiet_NaN(); projector.project(frame,result);
        for (auto v:result) QVERIFY(std::isfinite(v));
        frame.count=5; projector.project(frame,result);
        for (auto v:result) QCOMPARE(v,-120.0f);
    }
    void mailboxConcurrency() {
        SpectrumMailbox mailbox; std::atomic<bool> done{false};
        std::thread producer([&] {
            std::array<float,1025> bins;
            for (int frame=0;frame<10000;++frame) {
                bins.fill(float(frame)); mailbox.publish(bins.data(),int(bins.size()),48000);
            }
            done.store(true);
        });
        SpectrumFrame frame; bool torn=false; int received=0;
        while (!done.load()) if (mailbox.takeLatest(frame)) {
            ++received;
            for (int i=1;i<frame.count;++i) torn=torn || frame.db[i]!=frame.db[0];
        }
        producer.join();
        if (mailbox.takeLatest(frame)) ++received;
        QVERIFY(received>0); QVERIFY(!torn);
    }
    void mouseAndQSlider() {
        EqWidget widget; widget.resize(800,480); widget.setSpectrumEnabled(false);
        widget.setBands({Band{}}); widget.show(); QTest::qWait(70);
        QSignalSpy changed(&widget,&EqWidget::bandEdited), ended(&widget,&EqWidget::bandEditFinished);
        const auto start=widget.bandPosition(0).toPoint();
        QTest::mousePress(&widget,Qt::LeftButton,Qt::NoModifier,start);
        const QPoint end=start+QPoint(85,-30);
        QMouseEvent move(QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&widget,&move);
        QTest::mouseRelease(&widget,Qt::LeftButton,Qt::NoModifier,end);
        QVERIFY(widget.bands()[0].frequency>1000); QVERIFY(widget.bands()[0].gainDb>0);
        QVERIFY(changed.count()>0); QCOMPARE(ended.count(),1);
        QTest::mouseClick(&widget,Qt::LeftButton,Qt::NoModifier,widget.qControlRect().topRight().toPoint()+QPoint(0,24));
        QVERIFY(std::abs(widget.bands()[0].q-30)<1e-6);
        QTest::qWait(80); const auto idle=widget.paintedFrames(); QTest::qWait(100);
        QCOMPARE(widget.paintedFrames(),idle);
        widget.setSampleRate(8000); QVERIFY(widget.bands()[0].frequency<=3992);
    }
    void spectrumTrafficDoesNotRecomputeEq() {
        EqWidget widget; widget.resize(800,480); widget.setBands(std::vector<Band>(8));
        widget.show(); QTest::qWait(180); // ensure the static EQ overlay cache exists
        widget.startProfiling();
        const auto evaluations=widget.responseEvaluations();
        const auto paints=widget.paintedFrames();
        std::array<float,2049> bins; bins.fill(-90);
        int submissions=0; auto mailbox=widget.spectrumInput();
        QTimer producer;
        connect(&producer,&QTimer::timeout,&widget,[&] {
            bins[80]=float(-20+10*std::sin(++submissions*0.2));
            mailbox->publish(bins.data(),int(bins.size()),48000);
        });
        QElapsedTimer elapsed; elapsed.start(); producer.start(1); QTest::qWait(200); producer.stop();
        QCOMPARE(widget.responseEvaluations(),evaluations);
        QVERIFY(widget.paintedFrames()>paints);
        QVERIFY(widget.paintedFrames()-paints<=quint64(elapsed.elapsed()/30+3));
        // Spectrum frames should reuse the EQ bitmap after the quiet period.
        QCOMPARE(widget.profile()->phases[FrameProfile::Overlay].count,quint64(0));
        QCOMPARE(widget.profile()->phases[FrameProfile::Curves].count,quint64(0));
        QCOMPARE(widget.profile()->phases[FrameProfile::Controls].count,quint64(0));
        widget.stopProfiling();
    }
    void pcmFileSourcePublishesMeasuredFrames() {
        QTemporaryFile pcm;
        QVERIFY(pcm.open());
        constexpr int sampleRate = 44100;
        constexpr int sampleCount = 8192;
        for (int i = 0; i < sampleCount; ++i) {
            const auto sample = std::int16_t(12000.0 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * i / sampleRate));
            const char bytes[2] = {char(sample & 0xff), char((std::uint16_t(sample) >> 8) & 0xff)};
            QCOMPARE(pcm.write(bytes, 2), qint64(2));
        }
        pcm.flush();
        auto mailbox = std::make_shared<SpectrumMailbox>();
        PcmFileSpectrumSource source(mailbox);
        PcmFileSpectrumSource::Config config;
        config.path = pcm.fileName().toStdString();
        config.sampleRate = sampleRate;
        config.fftSize = 256;
        config.hopSize = 128;
        config.loop = false;
        QVERIFY(source.start(config));
        SpectrumFrame frame;
        bool received = false;
        QElapsedTimer wait;
        wait.start();
        while (!received && wait.elapsed() < 1500) {
            received = mailbox->takeLatest(frame);
            if (!received) QTest::qWait(5);
        }
        source.stop();
        QVERIFY(received);
        QCOMPARE(frame.count, 129);
        for (int i = 0; i < frame.count; ++i) QVERIFY(std::isfinite(frame.db[i]));
        const auto peak = std::max_element(frame.db.begin(), frame.db.begin() + frame.count);
        QVERIFY(peak - frame.db.begin() >= 5 && peak - frame.db.begin() <= 7);
        QVERIFY(frame.db[6] > -20.0f);
        const auto stats = source.stats();
        QVERIFY(stats.inputSamples > 0);
        QVERIFY(stats.fftFrames > 0);
        QVERIFY(stats.publishedFrames > 0);
        QVERIFY(stats.fftNanoseconds >= stats.fftMaxNanoseconds);
    }
    void touchPinchTransitionAndCancel() {
        EqWidget widget; widget.resize(800,480); widget.setBands({Band{}}); widget.show(); QTest::qWait(60);
        auto *device=QTest::createTouchDevice(QTouchDevice::TouchScreen);
        device->setMaximumTouchPoints(10);
        const auto start=widget.bandPosition(0).toPoint();
        QSignalSpy ended(&widget,&EqWidget::bandEditFinished);
        QSignalSpy started(&widget,&EqWidget::bandEditStarted);
        auto touch=QTest::touchEvent(&widget,device,false);
        touch.press(0,start,&widget).commit();
        QCOMPARE(started.count(),1);
        touch.stationary(0).press(1,start+QPoint(80,0),&widget).commit();
        const auto before=widget.bands()[0];
        touch.stationary(0).move(1,start+QPoint(160,0),&widget).commit();
        QVERIFY(widget.bands()[0].q<before.q*0.6);
        QCOMPARE(widget.bands()[0].frequency,before.frequency);
        QCOMPARE(widget.bands()[0].gainDb,before.gainDb);
        touch.stationary(0).release(1,start+QPoint(160,0),&widget).commit();
        const double frequency=widget.bands()[0].frequency;
        touch.move(0,start,&widget).commit();
        QVERIFY(std::abs(widget.bands()[0].frequency-frequency)<1e-6);
        touch.move(0,start+QPoint(25,-15),&widget).commit();
        QVERIFY(widget.bands()[0].frequency>frequency);
        QTouchEvent cancel(QEvent::TouchCancel,device); QApplication::sendEvent(&widget,&cancel);
        QCOMPARE(ended.count(),1); QVERIFY(ended.at(0).at(2).toBool());
        touch.release(0,start,&widget).commit();
    }
    void tenTouchesAndPrimaryReplacement() {
        EqWidget widget; widget.resize(800,480); widget.setBands({Band{}}); widget.show(); QTest::qWait(60);
        auto *device=QTest::createTouchDevice(QTouchDevice::TouchScreen);
        device->setMaximumTouchPoints(10);
        const auto start=widget.bandPosition(0).toPoint();
        auto sequence=QTest::touchEvent(&widget,device,false);
        QSignalSpy started(&widget,&EqWidget::bandEditStarted);
        sequence.press(0,start,&widget).commit();
        QCOMPARE(started.count(),1);
        sequence.stationary(0).press(1,start+QPoint(80,0),&widget).commit();
        sequence.stationary(0).stationary(1);
        for(int id=2;id<10;++id) sequence.press(id,start+QPoint(id*10,40),&widget);
        sequence.commit();
        const auto before=widget.bands()[0];
        sequence.release(0,start,&widget);
        for(int id=1;id<10;++id) sequence.stationary(id);
        sequence.commit();
        QCOMPARE(widget.bands()[0].q,before.q);
        QCOMPARE(widget.bands()[0].frequency,before.frequency);
        QTouchEvent cancel(QEvent::TouchCancel,device); QApplication::sendEvent(&widget,&cancel);
    }
    void filterIconsAndReadOnlyCoefficients() {
        for (int type=0;type<=12;++type) {
            const auto image=filterIcon(FilterType(type)).pixmap(28,28).toImage();
            bool ink=false;
            for (int y=0;y<image.height();++y) for (int x=0;x<image.width();++x) ink=ink || qAlpha(image.pixel(x,y))>0;
            QVERIFY(ink);
        }
        EqWidget widget; Band b; b.useCustomCoefficients=true; widget.setBands({b}); widget.show(); QTest::qWait(50);
        QSignalSpy changed(&widget,&EqWidget::bandEdited);
        QTest::mouseClick(&widget,Qt::LeftButton,Qt::NoModifier,widget.qControlRect().center().toPoint());
        QCOMPARE(changed.count(),0);
    }
};
QTEST_MAIN(Tests)
#include "test_qdsssp.moc"

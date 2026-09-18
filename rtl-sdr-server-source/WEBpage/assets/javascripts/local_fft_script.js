(function () {
    const ws_url = "ws://" + (window.location.hostname || "localhost") + ":7681";
    const ws_name = "fft_fast";
    const render_interval = 100; // ms

    const fft_colour = 'rgba(3,215,252,0.7)';
    const scale_db = 3276.8;
    const _start_freq = 490.5;

    let render_buffer = [];
    let render_busy = false;
    let render_timer;

    let el, canvas_jqel, ctx, canvasWidth, canvasHeight;
    let mouse_in_canvas = false;
    let mouse_x = 0;
    let mouse_y = 0;

    let signals = [];
    let freq_info = [];
    let beacon_strength = 0;

    const local_ws = new u16Websocket(ws_url, ws_name, render_buffer);

    $(function () {
        canvasHeight = 600;
        canvasWidth = $("#fft-col-2").width();

        el = document.getElementById('c2');
        canvas_jqel = $("#c2");

        initCanvas();
        updateFFT(null);

        canvas_jqel.on('mousemove', function (e) {
            mouse_in_canvas = true;

            const el_boundingRectangle = el.getBoundingClientRect();
            mouse_x = e.clientX - el_boundingRectangle.left;
            mouse_y = e.clientY - el_boundingRectangle.top;

            render_frequency_info(mouse_x, mouse_y);
            render_signal_box(mouse_x, mouse_y);
        });

        canvas_jqel.on('mouseleave', function () {
            mouse_in_canvas = false;
        });
    });

    function initCanvas() {
        $("#c2").attr("width", canvasWidth);
        $("#c2").attr("height", canvasHeight);

        ctx = el.getContext('2d');

        const devicePixelRatio = window.devicePixelRatio || 1;
        const backingStoreRatio = ctx.webkitBackingStorePixelRatio ||
            ctx.mozBackingStorePixelRatio ||
            ctx.msBackingStorePixelRatio ||
            ctx.oBackingStorePixelRatio ||
            ctx.backingStorePixelRatio || 1;
        const ratio = devicePixelRatio / backingStoreRatio;

        if (devicePixelRatio !== backingStoreRatio) {
            const oldWidth = el.width;
            const oldHeight = el.height;

            el.width = oldWidth * ratio;
            el.height = oldHeight * ratio;

            el.style.width = oldWidth + 'px';
            el.style.height = oldHeight + 'px';

            ctx.scale(ratio, ratio);
        }
    }

    function updateFFT(data) {
        let i;

        ctx.clearRect(0, 0, canvasWidth, canvasHeight);
        ctx.save();

        /* Draw Dashed Vertical Lines and headers */
        ctx.lineWidth = 1;
        ctx.strokeStyle = 'grey';
        ctx.setLineDash([5, 20]);
        ctx.font = "15px ABeeZee";
        ctx.fillStyle = "white";
        ctx.textAlign = "center";
        for (i = 0; i < 18; i += 2) {
            ctx.beginPath();
            ctx.moveTo((canvasWidth / 18) + i * (canvasWidth / 18), 25);
            ctx.lineTo((canvasWidth / 18) + i * (canvasWidth / 18), canvasHeight * (7 / 8));
            ctx.stroke();
            ctx.fillText("10.4" + (91 + (i * 0.5)), (canvasWidth / 18) + i * (canvasWidth / 18), 17);
        }

        /* Draw Horizontal Lines */
        ctx.lineWidth = 1;
        ctx.strokeStyle = 'grey';
        ctx.setLineDash([5, 10]);
        ctx.font = "12px ABeeZee";
        ctx.fillStyle = "white";
        ctx.textAlign = "center";
        for (i = 1; i <= 4; i++) {
            const linePos = (i * (canvasHeight / 4)) - (canvasHeight / 6);
            ctx.beginPath();
            ctx.moveTo(0 + 35, linePos);
            ctx.lineTo(canvasWidth - 35, linePos);
            ctx.stroke();
            if (i !== 4) {
                ctx.fillText((5 * (4 - i)) + "dB", 17, linePos + 4);
                ctx.fillText((5 * (4 - i)) + "dB", canvasWidth - 17, linePos + 4);
            }
        }

        /* Draw Minor Horizontal Lines */
        ctx.lineWidth = 1;
        ctx.strokeStyle = 'grey';
        ctx.setLineDash([1, 10]);
        for (i = 1; i < 20; i++) {
            if (i % 5 !== 0) {
                const linePos = (i * (canvasHeight / 20)) - (canvasHeight / 6);
                ctx.beginPath();
                ctx.moveTo(0 + 10, linePos);
                ctx.lineTo(canvasWidth - 10, linePos);
                ctx.stroke();
            }
        }

        ctx.restore();

        /* Draw Band Splits */
        ctx.lineWidth = 1;
        ctx.strokeStyle = 'grey';

        function draw_divider(frequency, height) {
            ctx.beginPath();
            ctx.moveTo((frequency - _start_freq) * (canvasWidth / 9), canvasHeight * height);
            ctx.lineTo((frequency - _start_freq) * (canvasWidth / 9), canvasHeight * (7.9 / 8));
            ctx.stroke();
        }

        draw_divider(492.5, (7.1 / 8.0));
        draw_divider(497.0, (7.325 / 8.0));

        ctx.fillStyle = 'grey';

        function draw_channel(center_frequency, bandwidth, line_height) {
            const rolloff = 1.35 / 2.0;

            if (freq_info.length === 44) freq_info = [];
            freq_info.push({
                x1: ((center_frequency - (rolloff * bandwidth)) - _start_freq) * (canvasWidth / 9),
                x2: ((center_frequency + (rolloff * bandwidth)) - _start_freq) * (canvasWidth / 9),
                y: canvasHeight * line_height,
                center_frequency: center_frequency,
                bandwidth: bandwidth
            });

            ctx.fillRect(((center_frequency - (rolloff * bandwidth)) - _start_freq) * (canvasWidth / 9), canvasHeight * line_height, 2 * (rolloff * bandwidth) * (canvasWidth / 9), 5);
        }

        for (let f = 493.25; f <= 496.25; f = f + 1.5) {
            draw_channel(f, 1.0, (7.475 / 8));
        }
        for (let f = 492.75; f <= 499.25; f = f + 0.5) {
            draw_channel(f, 0.333, (7.25 / 8));
        }
        for (let f = 492.75; f <= 499.25; f = f + 0.25) {
            draw_channel(f, 0.125, (7.025 / 8));
        }

        ctx.restore();

        /* Annotate Bands */
        ctx.font = "30px ABeeZee";
        ctx.fillStyle = "rgb(252,252,252)";
        ctx.textAlign = "center";
        ctx.fillText("Local Feed (Python App)", canvasWidth / 2, 80);
        ctx.font = "15px ABeeZee";
        ctx.fillStyle = "white";
        ctx.textAlign = "center";

        ctx.fillText("A71A  Beacon", ((491.5) - _start_freq) * (canvasWidth / 9), canvasHeight - 45);
        ctx.fillText("10 491 500", ((491.5) - _start_freq) * (canvasWidth / 9), canvasHeight - 28);
        ctx.font = "12px ABeeZee";
        ctx.fillText("1.5MS/s QPSK, 4/5", ((491.5) - _start_freq) * (canvasWidth / 9), canvasHeight - 12);
        ctx.font = "15px ABeeZee";
        ctx.fillText("Wide & Narrow Section", ((494.75) - _start_freq) * (canvasWidth / 9), canvasHeight - 12);
        ctx.fillText("Narrow Section", ((498.25) - _start_freq) * (canvasWidth / 9), canvasHeight - 12);
        ctx.restore();

        /* Draw FFT */
        if (data != null) {
            const start_height = canvasHeight * (7 / 8);
            const data_length = data.length;

            ctx.lineWidth = 1;
            ctx.strokeStyle = fft_colour;
            for (i = 0; i < canvasWidth; i++) {
                const sample_index = (i * data_length) / canvasWidth;
                const sample_index_f = sample_index | 0;
                let sample = data[sample_index_f]
                    + (sample_index - sample_index_f) * (data[sample_index_f + 1] - data[sample_index_f]);
                sample = (sample / 65536.0);

                if (sample > (1 / 8)) {
                    ctx.beginPath();
                    ctx.moveTo(i, start_height);
                    ctx.lineTo(i, canvasHeight - (Math.min(sample, 1.0) * canvasHeight));
                    ctx.stroke();
                }
            }
            ctx.restore();
        } else {
            ctx.font = "15px ABeeZee";
            ctx.fillStyle = "white";
            ctx.textAlign = "center";
            ctx.fillText("Waiting for local feed..", (canvasWidth / 2) + (canvasWidth / 35), (3 * (canvasHeight / 4)) - ((1.1 / 6) * canvasHeight));
            ctx.restore();
        }
    }

    function align_symbolrate(width) {
        if (width < 0.022) return 0;
        if (width < 0.060) return 0.035;
        if (width < 0.086) return 0.066;
        if (width < 0.185) return 0.125;
        if (width < 0.277) return 0.250;
        if (width < 0.388) return 0.333;
        if (width < 0.700) return 0.500;
        if (width < 1.2) return 1.000;
        if (width < 1.6) return 1.500;
        if (width < 2.2) return 2.000;
        return Math.round(width * 5) / 5.0;
    }

    function print_symbolrate(symrate) {
        if (symrate < 0.7) {
            return Math.round(symrate * 1000) + "KS";
        }
        return (Math.round(symrate * 10) / 10) + "MS";
    }

    function print_frequency(freq, symrate) {
        if (symrate < 0.7) {
            return "'" + (Math.round(freq * 80) / 80.0).toFixed(3);
        }
        return "'" + (Math.round(freq * 40) / 40.0).toFixed(3);
    }

    function is_overpower(signal_strength, signal_bw) {
        if (beacon_strength !== 0) {
            if (signal_bw < 0.7) return false;
            if (signal_strength > (beacon_strength - (0.75 * scale_db))) return true;
        }
        return false;
    }

    function detect_signals(fft_data) {
        const noise_level = 11000;
        const signal_threshold = 16000;

        let in_signal = false;
        let start_signal, end_signal, mid_signal, strength_signal, signal_bw, signal_freq;
        let text_x_position;

        signals = [];

        for (let i = 2; i < fft_data.length; i++) {
            if (!in_signal) {
                if ((fft_data[i] + fft_data[i - 1] + fft_data[i - 2]) / 3.0 > signal_threshold) {
                    in_signal = true;
                    start_signal = i;
                }
            } else {
                if ((fft_data[i] + fft_data[i - 1] + fft_data[i - 2]) / 3.0 < signal_threshold) {
                    in_signal = false;
                    end_signal = i;

                    let acc = 0;
                    let acc_i = 0;
                    for (let j = (start_signal + (0.3 * (end_signal - start_signal))) | 0; j < start_signal + (0.7 * (end_signal - start_signal)); j++) {
                        acc += fft_data[j];
                        acc_i++;
                    }
                    strength_signal = acc / acc_i;

                    for (let j = start_signal; (fft_data[j] - noise_level) < 0.75 * (strength_signal - noise_level); j++) {
                        start_signal = j;
                    }
                    for (let j = end_signal; (fft_data[j] - noise_level) < 0.75 * (strength_signal - noise_level); j--) {
                        end_signal = j;
                    }

                    mid_signal = start_signal + ((end_signal - start_signal) / 2.0);
                    signal_bw = align_symbolrate((end_signal - start_signal) * (9.0 / fft_data.length));
                    signal_freq = 490.5 + (((mid_signal + 1) / fft_data.length) * 9.0);

                    signals.push({
                        start: (start_signal / fft_data.length) * canvasWidth,
                        end: (end_signal / fft_data.length) * canvasWidth,
                        top: canvasHeight - ((strength_signal / 65536) * canvasHeight),
                        frequency: 10000 + signal_freq,
                        symbolrate: 1000.0 * signal_bw
                    });

                    if (signal_freq < 492.0) {
                        if (signal_bw >= 1.0) {
                            beacon_strength = strength_signal;
                        }
                        continue;
                    }

                    if (signal_bw !== 0) {
                        text_x_position = (mid_signal / fft_data.length) * canvasWidth;
                        if (text_x_position > (0.92 * canvasWidth)) {
                            text_x_position = canvasWidth - 55;
                        }

                        ctx.font = "14px ABeeZee";
                        ctx.fillStyle = "white";
                        ctx.textAlign = "center";
                        if (!is_overpower(strength_signal, signal_bw)) {
                            ctx.fillText(
                                print_symbolrate(signal_bw) + ", " + print_frequency(signal_freq, signal_bw),
                                text_x_position,
                                canvasHeight - ((strength_signal / 65536) * canvasHeight) - 16
                            );
                            ctx.restore();
                        } else {
                            ctx.fillText("[over-power]", text_x_position, canvasHeight - ((strength_signal / 65536) * canvasHeight) - 16);
                            ctx.restore();

                            ctx.lineWidth = 2;
                            ctx.strokeStyle = 'white';
                            ctx.setLineDash([4, 4]);
                            ctx.beginPath();
                            ctx.moveTo((start_signal / fft_data.length) * canvasWidth, canvasHeight * (1 - ((beacon_strength - (1.0 * scale_db)) / 65536)));
                            ctx.lineTo((end_signal / fft_data.length) * canvasWidth, canvasHeight * (1 - ((beacon_strength - (1.0 * scale_db)) / 65536)));
                            ctx.stroke();
                            ctx.setLineDash([]);
                            ctx.restore();
                        }
                    }
                }
            }
        }

        if (in_signal) {
            end_signal = fft_data.length;
            let acc = 0;
            let acc_i = 0;
            for (let j = (start_signal + (0.3 * (end_signal - start_signal))) | 0; j < start_signal + (0.7 * (end_signal - start_signal)); j++) {
                acc += fft_data[j];
                acc_i++;
            }
            strength_signal = acc / acc_i;

            ctx.font = "14px ABeeZee";
            ctx.fillStyle = "white";
            ctx.textAlign = "center";
            ctx.fillText("[out-of-band]", (canvasWidth - 55), canvasHeight - ((strength_signal / 65536) * canvasHeight) - 16);
            ctx.restore();
        }

        if (mouse_in_canvas) {
            render_frequency_info(mouse_x, mouse_y);
            render_signal_box(mouse_x, mouse_y);
        }
    }

    function render_signal_box(local_mouse_x, local_mouse_y) {
        if (local_mouse_y < (canvasHeight * 7 / 8)) {
            for (let i = 0; i < signals.length; i++) {
                if (local_mouse_x > signals[i].start
                    && local_mouse_x < signals[i].end
                    && local_mouse_y > signals[i].top) {
                    ctx.lineWidth = 1;
                    ctx.strokeStyle = 'white';

                    ctx.beginPath();
                    ctx.moveTo(signals[i].start, canvasHeight * (7 / 8));
                    ctx.lineTo(signals[i].start, signals[i].top);
                    ctx.stroke();

                    ctx.beginPath();
                    ctx.moveTo(signals[i].start, signals[i].top);
                    ctx.lineTo(signals[i].end, signals[i].top);
                    ctx.stroke();

                    ctx.beginPath();
                    ctx.moveTo(signals[i].end, canvasHeight * (7 / 8));
                    ctx.lineTo(signals[i].end, signals[i].top);
                    ctx.stroke();

                    if ((beacon_strength > 0) && (signals[i].start > canvasWidth / 8)) {
                        ctx.font = "16px ABeeZee";
                        ctx.fillStyle = "#54e50d";
                        ctx.textAlign = "center";

                        const db_per_pixel = ((canvasHeight * 7 / 8) - (canvasHeight / 12)) / 15;
                        const beacon_strength_pixel = canvasHeight - ((beacon_strength / 65536) * canvasHeight);

                        const x = ((beacon_strength_pixel - signals[i].top) / db_per_pixel).toFixed(1);
                        let text_to_display = x.toString() + " dBb";
                        let label_y_pos_offset = -20;
                        if (x > 0) {
                            text_to_display = "+" + x.toString() + " dBb";
                            ctx.fillStyle = "#b40606";
                            label_y_pos_offset = 70;
                        }

                        ctx.fillText(text_to_display,
                            signals[i].start - ((signals[i].start - signals[i].end) / 2),
                            (canvasHeight * 7 / 8) - (7 * ((canvasHeight * 7 / 8) - signals[i].top) / 8 + label_y_pos_offset));
                    }

                    ctx.restore();
                    return;
                }
            }
        }
    }

    function render_frequency_info(local_mouse_x, local_mouse_y) {
        let display_triggered = false;
        if (local_mouse_y > (canvasHeight * 7 / 8)) {
            for (let i = 0; i < freq_info.length; i++) {
                const xd1 = freq_info[i].x1;
                const xd2 = freq_info[i].x2;
                const yd = freq_info[i].y;
                if ((local_mouse_x > xd1 - 1) && (local_mouse_x < xd2 + 1)
                    && (local_mouse_y > yd - 5) && (local_mouse_y < yd + 5)) {
                    el.title = "Downlink: " + (10000.00 + freq_info[i].center_frequency) +
                        " MHz\n( IF " + ((10000.00 + freq_info[i].center_frequency) - 9750) +
                        " MHz)\nUplink: " + (1910.50 + freq_info[i].center_frequency) +
                        " MHz\nSymbol Rate: " + ((freq_info[i].bandwidth === 0.125) ? "125/66/33 Ksps" :
                            (freq_info[i].bandwidth === 0.333) ? (freq_info[i].center_frequency < 497.0 ? "500/333/250 Ksps" : "333/250 Ksps") : "1 Msps");

                    ctx.fillStyle = '#dca85c';
                    ctx.fillRect(xd1, yd, xd2 - xd1, 5);
                    display_triggered = true;
                    break;
                }
            }
        }
        if (!display_triggered) {
            el.title = "";
        }
    }

    function render_fft() {
        if (!render_busy) {
            render_busy = true;
            if (render_buffer.length > 0) {
                const data_frame = render_buffer.shift();
                updateFFT(data_frame);
                detect_signals(data_frame);

                if (render_buffer.length > 2) {
                    render_buffer.splice(0, render_buffer.length - 2);
                }
            }
            render_busy = false;
        } else {
            console.log("Local spectrum: slow render blocking next frame, configured interval is ", render_interval);
        }
    }

    render_timer = setInterval(render_fft, render_interval);

    let previousHeight = window.innerHeight;
    let previousWidth = window.innerWidth;
    function checkResize() {
        if (previousHeight !== window.innerHeight || previousWidth !== window.innerWidth) {
            canvasHeight = 600;
            canvasWidth = $("#fft-col-2").width();
            initCanvas();

            previousHeight = window.innerHeight;
            previousWidth = window.innerWidth;
        }
    }

    window.addEventListener("resize", checkResize, false);
})();
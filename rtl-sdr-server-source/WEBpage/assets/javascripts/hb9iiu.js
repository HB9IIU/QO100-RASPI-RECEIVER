function update(picker) {
    fft_colour = picker.toRGBAString()
    if (storageSupport) {
        localStorage.wb_fft_colour = fft_colour;
    }
    jscolor.trigger('input'); // triggers 'onInput' on all color pickers when they are ready
}
$(document).ready(function () {
    var slider = document.getElementById("customRange1");
    var color0 = document.querySelector(".color0");
    var color1 = document.querySelector(".color1");
    var color2 = document.querySelector(".color2");
    var random = document.getElementById("random");
    var auto = document.getElementById("auto");
    var stop = document.getElementById("stop");
    var fft_background_colour = 'black';


    slider.oninput = function () {
        var slider_value = this.value / 100
        var new_color = fft_colour.substring(0, fft_colour.lastIndexOf(",") + 1) + slider_value + ")";
        fft_colour = new_color
        if (storageSupport) {
            localStorage.wb_fft_colour = fft_colour;
        }
    }
    //Set fft background colour
    fft_background_colour = localStorage.spectrum_background_colour;

    if (fft_background_colour == undefined) {
        fft_background_colour = "background: linear-gradient(to top,#282f28,#2ca6ce)" //default
    }

    document.getElementById("c").style = fft_background_colour;
    initCanvas()

    function setGradient_background() {
        newColor = "background: linear-gradient(to top," + color1.value + "," + color2.value + ")"
        document.getElementById("c").style = newColor;
        if (storageSupport) {
            localStorage.spectrum_background_colour = newColor;
        }
        initCanvas()
    }

    function setGradient_spectrum() {
        var argb = hexToRgb(color0.value)
        var slider_value = (document.getElementById("customRange1").value) / 100
        var new_color = "rgba(" + argb.r + "," + argb.g + "," + argb.b + "," + slider_value + ")"
        fft_colour = new_color
        if (storageSupport) {
            localStorage.wb_fft_colour = fft_colour;
        }
    }

    function randomColor() {
        var hexCode = "";
        var hexValues = "0123456789abcdef";
        for (var i = 0; i < 6; i++) {
            hexCode += hexValues.charAt(Math.floor(Math.random() * hexValues.length));
        }
        newColor = "#" + hexCode
        return newColor;
    }

    function hexToRgb(hex) {
        var result = /^#?([a-f\d]{2})([a-f\d]{2})([a-f\d]{2})$/i.exec(hex);
        return result ? {
            r: parseInt(result[1], 16),
            g: parseInt(result[2], 16),
            b: parseInt(result[3], 16)
        } : null;
    }

    function randomGradient() {
        color1.value = randomColor();
        color2.value = randomColor();
        setGradient_background();
    }

    color0.addEventListener("input", setGradient_spectrum)
    color1.addEventListener("input", setGradient_background)
    color2.addEventListener("input", setGradient_background)
    random.addEventListener("click", randomGradient)
    auto.addEventListener("click", function () {
        randomGradient();
        interval = setInterval(randomGradient, 2000);
    })
    stop.onmousedown = function () {
        clearInterval(interval)
    }


    $("input[name=flexRadioDefault]").change(function () {
        //console.log(this.id)
        render_interval = render_interval_map[this.id];
        clearInterval(render_timer);
        render_timer = setInterval(render_fft, render_interval);
        fft_ws.changeName(this.id);
        if (storageSupport) {
            localStorage.wb_fft_speed = this.id;
        }
    });
    document.getElementById("fullscreen-link-button")?.addEventListener("click", fft_fullscreen);

    if (render_interval == 250) {
        radiobtn = document.getElementById("fft");
        radiobtn.checked = true;
    } else {
        radiobtn = document.getElementById("fft_fast");
        radiobtn.checked = true;
    }


});

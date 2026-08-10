
var canvasWidth=256;
var canvasHeight=256;
var iq_margin = 0.2;

var iq_ctx
var iq_ratio;

$(document).ready(function()
{
    $("#canvas-constellation").attr( "width", canvasWidth );
    $("#canvas-constellation").attr( "height", canvasHeight );

    el = document.getElementById('canvas-constellation');

    iq_ctx = el.getContext('2d');

    devicePixelRatio = window.devicePixelRatio || 1,
    backingStoreRatio = iq_ctx.webkitBackingStorePixelRatio ||
                      iq_ctx.mozBackingStorePixelRatio ||
                      iq_ctx.msBackingStorePixelRatio ||
                      iq_ctx.oBackingStorePixelRatio ||
                      iq_ctx.backingStorePixelRatio || 1,
    iq_ratio = devicePixelRatio / backingStoreRatio;

    if (devicePixelRatio !== backingStoreRatio)
    {
        var oldWidth = el.width;
        var oldHeight = el.height;

        el.width = oldWidth * iq_ratio;
        el.height = oldHeight * iq_ratio;

        el.style.width = oldWidth + 'px';
        el.style.height = oldHeight + 'px';

        iq_ctx.scale(ratio, iq_ratio);
    }
});

function quickRound(f)
{
    return (0.5 + f) | 0;
}


function drawIQGrid(ctx)
{
    /* Cross */
    ctx.strokeStyle = 'grey';
    ctx.lineWidth = 1;
    
    ctx.beginPath();

    ctx.moveTo(0,           quickRound(canvasHeight/2));
    ctx.lineTo(canvasWidth, quickRound(canvasHeight/2));
    
    ctx.moveTo(quickRound(canvasWidth/2), 0);
    ctx.lineTo(quickRound(canvasWidth/2), canvasHeight);
    
    ctx.stroke();
    
    ctx.stroke();
}

function constellation_draw(data)
{
    var new_canvas = document.createElement('canvas');
    new_canvas.width = canvasWidth;
    new_canvas.height = canvasHeight;
    var new_ctx = new_canvas.getContext('2d');

    /* Draw Grid Lines */
    drawIQGrid(new_ctx);

    /* Draw IQ */
    new_ctx.fillStyle='#009A00';
    new_ctx.beginPath();
    data.forEach(function(point)
    {
        //console.log(normaliseIQ(point[0]), normaliseIQ(point[1]))
        new_ctx.fillRect(
            quickRound(point[0]*iq_ratio),
            quickRound(point[1]*iq_ratio),
            quickRound(2/iq_ratio),
            quickRound(2/iq_ratio)
        );
    });
    new_ctx.stroke();

    iq_ctx.clearRect(0, 0, canvasWidth, canvasHeight);
    iq_ctx.drawImage(new_canvas, 0, 0);
}
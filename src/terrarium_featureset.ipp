#include <mapnik/debug.hpp>
#include <mapnik/image.hpp>
#include <mapnik/raster.hpp>
#include <mapnik/view_transform.hpp>
#include <mapnik/image_reader.hpp>
#include <mapnik/image_util.hpp>
#include <mapnik/feature_factory.hpp>
#include <mapnik/util/variant.hpp>

#include "terrarium_featureset.hpp"

using mapnik::query;
using mapnik::image_reader;
using mapnik::feature_ptr;
using mapnik::image_rgba8;
using mapnik::raster;
using mapnik::feature_factory;

namespace mapnik {

terrarium_featureset::terrarium_featureset(box2d<double> const& extent,
                                           query const& q,
                                           std::shared_ptr<mapnik::image_reader> image_reader)
    : feature_id_(1),
      ctx_(std::make_shared<mapnik::context_type>()),
      extent_(extent),
      bbox_(q.get_bbox()),
      filter_factor_(q.get_filter_factor()),
      image_reader_(image_reader)
{
}

terrarium_featureset::~terrarium_featureset()
{
}

double height_val(uint32_t pixel) {
    uint8_t red = pixel & 0xff;
    uint8_t green = (pixel >> 8) & 0xff;
    uint8_t blue = (pixel >> 16) & 0xff;
    // https://github.com/tilezen/joerd/blob/master/docs/formats.md
    return (red * 256 + green + blue / 256) - 32768;
}

uint32_t pxl_from_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint32_t val = (a << 24);
    val = val | (b << 16);
    val = val | (g << 8);
    val = val | r;
    return val;
}


uint32_t bytes(mapnik::color c) {
    uint32_t val = (c.alpha() << 24);
    val = val | (c.blue() << 16);
    val = val | (c.green() << 8);
    val = val | c.red();
    return val;
}

// use signed integers, can be -1
double get_height(mapnik::image_rgba8 const& input,int32_t row,int32_t col) {
    return height_val(input.get_row(row+2)[col+2]);
}

void process_heightmap(mapnik::image_rgba8 const& input, mapnik::image_rgba8 &output) {
    for (int32_t row = 0; row < 512; row++) {
        uint32_t *buf = new uint32_t[512];
        for (int32_t col = 0; col < 512; col++) {
            double hgt = get_height(input,row,col);
            double frac = hgt / 1000; // arbitrary number
            if (frac < 0) frac = 0.0;
            if (frac > 1) frac = 1.0;
            frac = frac * 255;
            uint8_t v = frac;
            buf[col] = pxl_from_rgba(0,0,255,v);
        }
        output.set_row(row, buf, 512);
    }
}

mapnik::color blend(mapnik::color color1, mapnik::color color2) {
    uint8_t red = color1.red() + color2.red();
    uint8_t green = color1.green() + color2.green();
    uint8_t blue = color1.blue() + color2.blue();
    return mapnik::color{red,green,blue}; 
}

// see implementation from:
// https://observablehq.com/@sahilchinoy/hillshader
void process_hillshade(mapnik::image_rgba8 const& input, mapnik::image_rgba8 &output) {
    for (int32_t row = 0; row < 512; row++) {
        uint32_t *buf = new uint32_t[512];
        for (int32_t col = 0; col < 512; col++) {
            double hgt = get_height(input,row,col);

            double dzdx = get_height(input,row,col+1) - get_height(input,row,col-1);
            double dzdy = get_height(input,row+1,col) - get_height(input,row-1,col);
            double slope = atan(0.2 * sqrt(pow(dzdx,2) + pow(dzdy,2)));
            double aspect = atan2(-dzdy, -dzdx);

            double azimuth1 = 315.0;
            double elevation1 = 45.0;
            double elev1 = elevation1 * M_PI / 180.0;
            double luminance1 = cos(M_PI * .5 - aspect - (azimuth1 - 90.0) * M_PI / 180.0) * sin(slope) * sin(M_PI * .5 - elev1) + cos(slope) * cos(M_PI * .5 - elev1);
            if (luminance1 < 0) luminance1 = 0;
            luminance1 = sqrt(luminance1 * 0.8 + 0.2);
            luminance1 = luminance1 * 255;
            mapnik::color color1{(uint8_t)luminance1,(uint8_t)(luminance1/2),0};

            double azimuth2 = 225.0;
            double elevation2 = 45.0;
            double elev2 = elevation2 * M_PI / 180.0;
            double luminance2 = cos(M_PI * .5 - aspect - (azimuth2 - 90.0) * M_PI / 180.0) * sin(slope) * sin(M_PI * .5 - elev2) + cos(slope) * cos(M_PI * .5 - elev2);
            if (luminance2 < 0) luminance2 = 0;
            luminance2 = sqrt(luminance2 * 0.8 + 0.2);
            luminance2 = luminance2 * 255;
            mapnik::color color2{0,(uint8_t)(luminance2/2),(uint8_t)luminance2};
            mapnik::color color3 = blend(color1,color2);

            double alpha = (hgt - 20) / 100 * 255;
            if (alpha > 255) alpha = 255;
            if (alpha < 0) alpha = 0;
            color3.set_alpha(alpha);
            buf[col] = bytes(color3);
        }
        output.set_row(row, buf, 512);
    }
}

feature_ptr terrarium_featureset::next()
{
    if (done) return feature_ptr();
    feature_ptr feature(feature_factory::create(ctx_,feature_id_++));
    try
    {
        // TODO for zooms > 16 need to reintroduce overzooming
        mapnik::image_any input = image_reader_->read(0, 0, 516, 516);
        mapnik::image_rgba8 output(512,512);
        auto const &input_img = input.get<mapnik::image_rgba8>();
        //process_heightmap(input_img,output);
        process_hillshade(input_img,output);
        mapnik::raster_ptr raster = std::make_shared<mapnik::raster>(extent_, extent_, std::move(output), filter_factor_);
        feature->set_raster(raster);
    }
    catch (mapnik::image_reader_exception const& ex)
    {
        MAPNIK_LOG_ERROR(raster) << "Terrarium: image reader exception caught: " << ex.what();
    }
    catch (std::exception const& ex)
    {
        MAPNIK_LOG_ERROR(raster) << "Terrarium: " << ex.what();
    }
    catch (...)
    {
        MAPNIK_LOG_ERROR(raster) << "Terrarium: exception caught";
    }

    done = true;
    return feature;
}

}
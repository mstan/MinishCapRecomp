#include "mods/minish_definitive14.h"
#include "sha1.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
std::vector<uint8_t> read(const char* path) {
    std::ifstream in(path,std::ios::binary);
    CHECK(in.good()); return {std::istreambuf_iterator<char>(in),{}};
}
int main(int argc,char** argv) {
    CHECK(argc==3);
    const auto stock=read(argv[1]), expected=read(argv[2]);
    auto image=stock; const char* error=nullptr;
    CHECK(minish::prepare_definitive14_image(image.data(),image.size(),&error));
    CHECK(image==expected);
    CHECK(gba::sha1(image.data(),image.size()).hex()=="5de00d88ecc367c9d374c6b2601caa51a17ca743");
    CHECK(std::vector<uint8_t>(image.begin(),image.begin()+0xC0)==std::vector<uint8_t>(stock.begin(),stock.begin()+0xC0));
    const auto prepared=image;
    CHECK(!minish::prepare_definitive14_image(image.data(),image.size(),&error));
    CHECK(image==prepared);
    image=stock; image[0x100]^=1; const auto wrong=image;
    CHECK(!minish::prepare_definitive14_image(image.data(),image.size(),&error));
    CHECK(image==wrong);
    image=stock;
    CHECK(!minish::prepare_definitive14_image(image.data(),image.size()-1,&error));
    CHECK(image==stock);
    CHECK(!minish::prepare_definitive14_image(nullptr,stock.size(),&error));
    std::puts("Built-in DE1.4 exact image/header, no IPS, wrong cartridge/size/repeated application rejection PASS");
}

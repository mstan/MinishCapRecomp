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
    const auto stock=read(argv[1]), exact=read(argv[2]);
    auto image=stock, patch=exact; const char* error=nullptr;
    CHECK(minish::apply_definitive14_patch(image.data(),image.size(),patch.data(),patch.size(),&error));
    CHECK(gba::sha1(image.data(),image.size()).hex()=="5de00d88ecc367c9d374c6b2601caa51a17ca743");
    CHECK(std::vector<uint8_t>(image.begin(),image.begin()+0xC0)==std::vector<uint8_t>(stock.begin(),stock.begin()+0xC0));
    image=stock; patch.back()^=1;
    CHECK(!minish::apply_definitive14_patch(image.data(),image.size(),patch.data(),patch.size(),&error));
    CHECK(image==stock);
    patch=exact; image=stock; image[0x100]^=1; const auto wrong=image;
    CHECK(!minish::apply_definitive14_patch(image.data(),image.size(),patch.data(),patch.size(),&error));
    CHECK(image==wrong);
    image=stock;
    CHECK(!minish::apply_definitive14_patch(image.data(),image.size(),patch.data(),patch.size()-1,&error));
    CHECK(image==stock);
    std::puts("Exact Definitive Edition patch/output/header and wrong asset/cartridge/size rejection PASS");
}

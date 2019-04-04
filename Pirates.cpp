//
//  Pirates.cpp
//  pirates_savegame_editor
//
//  Created by Langsdorf on 3/30/19.
//  Copyright © 2019 Langsdorf. All rights reserved.
//

#include "Pirates.hpp"
#include "LineDecoding.hpp"
#include <iostream>
#include <string>
#include <regex>
#include <sys/stat.h>
#include <fstream>
#include <map>
#include <optional>
#include <vector>
#include <sstream>
#include <iomanip>
using namespace std;

int index_from_linecode (string line_code);

// The savegame file has variable length parts at the beginning and end,
// and a huge fixed length section in the middle. Once we hit the start of the fixed length section,
// it makes sense to peek far ahead to read the starting year, so that it can be used in all of the datestamps.
const string start_of_fixed_length_section = "Personal_0";

enum translation_type : char { TEXT0, TEXT8, HEX, INT, BINARY, SHORT, CHAR, LCHAR, mFLOAT, uFLOAT, FMAP, SMAP, CMAP, BULK, ZERO };

bool is_world_map(translation_type m) {
    // world maps get special handling.
    return (m==SMAP || m==CMAP || m==FMAP);
}

// The translation types have one character abbreviations in the pst file.
const map<translation_type,string> char_for_method = {
    {TEXT0,"t"}, {TEXT8,"t"}, {INT, "V"}, {HEX, "h"}, {BINARY, "B"}, {SHORT, "s"}, {CHAR, "C"}, {FMAP, "M"}, {BULK, "H"},
    {ZERO,"x"}, {uFLOAT,"G"}, {mFLOAT, "g"},{LCHAR, "c"}, {SMAP, "m"}, {CMAP, "MM"},
};

const map<translation_type,char> size_for_method = {
    {TEXT0,0}, {TEXT8,8}, {INT,4}, {HEX,4}, {BINARY,1}, {SHORT,2}, {CHAR,1}, {ZERO,0},
    {uFLOAT,4}, {BULK, 4}, {LCHAR, 1}, {mFLOAT, 4},
};

struct section {
    string name;
    int count;                        // i.e. how many lines to divide into
    int bytes_per_line;               // Except for text.
    translation_type method = BULK;    // Decode method for this section.
};

struct feature {
    string line_code;                   // world_map features get printed as separate lines
    unsigned char v;
};

// Main description of contents and size of each section, in order.
//    sections with single letter names are generally not understood.
//
// Sections will then be broken up further and further, recurvively. There are two key rules
//   1. Dewey Decimal Rule: You can break up a section into subsections, as long as their bytes add up to that of the parent.
//                          This allows more translation of bytes within a subsection without renumbering any other sections.
//   2. Backward Compatible: The pst file gives enough information about each line to restore the pirates_savegame file,
//                           even if the pst decoding has changed.
//
const vector<section> section_vector = {
    {"Intro",             6,       4,  INT },
    {"CityName",        128,       8,  TEXT8 },
    {"Personal",         57,       4,  INT },
    {"Ship",            128,    1116, },
    {"f",               128,    1116, },
    {"City",            128,      32, },
    {"CityInfo",        128,     148, },
    {"Log",            1000,      28, },
    {"j",                 1,       4, HEX },
    {"e",                30,      32, }, // e is really at least 3 parts: unknown, peace_and_war, and date and age.
    {"Quest",            64,      32, },
    {"LogCount",          1,       4, INT},
    {"TopoMap",         462,     586, },
    {"FeatureMap",      462,     293, FMAP},
    {"TreasureMap",       4,     328, },
    {"SailingMap",      462,     293, SMAP},
    {"vv",              256,      12, },
    {"vvv",               2,       4, INT},
    {"Top10",            10,      28, },
    {"d",                 1,      36, ZERO},
    {"Villain",          28,      36, },
    {"t",                 1,      120, },
    {"CityLoc",         128,      16, },
    {"CoastMap",        462,     293, CMAP},
    {"k",                 8,       4, INT},
    {"LandingParty",      8,      32, },
    {"m",                 1,      12, },
    {"ShipName",          8,       8,  TEXT8 },
    {"Skill",             1,       4, INT },
    
};

struct subsection_info {
    translation_type method;
    int byte_count=size_for_method.at(method);
    int multiplier=1;
};

// Split up a section into multiple lines of identically sized smaller types, assuming that they will
// use the default byte counts for that type.
// The size of the new translation_type should divide evenly into the original line size (this is checked at runtime)
map<string,translation_type> subsection_simple_decode = {
    {"Intro_0",       TEXT0},
    {"Intro_3",       HEX},
    {"Personal_2",    BINARY},
    {"Personal_5",    SHORT},
    {"Personal_6",    SHORT},
    {"Personal_9",    SHORT},
    {"Personal_10",   SHORT},
    {"Personal_18",   BINARY},
    {"Personal_45",   SHORT},
    {"Personal_46",   SHORT},
    {"Personal_47",   CHAR},
    {"Personal_48",   CHAR},
    {"Personal_49",   CHAR},
    {"Personal_50",   CHAR},
    {"Ship_x_2",      SHORT},
    {"Ship_x_3",      SHORT},
    {"Ship_x_5",      SHORT},
    {"Ship_x_5_4",    BINARY},
    {"Ship_x_6",      SHORT},
    {"City_x",        INT},
    {"City_x_2",      BINARY},
    {"City_x_4",      BULK},
    {"City_x_7",      BULK},
    {"CityInfo_x_0_2", SHORT},
    {"CityInfo_x_0_3", SHORT},
    {"CityInfo_x_3",   SHORT},
    // {"Log_x_1",   BINARY }   // Pleased/Greatly Pleased
    // {"Log_x_2",   BINARY }   // Offended
    {"e_x",           INT},
    {"Quest_x",       INT},
    {"TreasureMap_x", INT},
    {"vv_x",          INT},
    {"Villain_x_4",   BINARY},
    {"t_7",           INT},
    {"LandingParty_0",uFLOAT},
    {"LandingParty_1",uFLOAT},
    {"LandingParty_x",HEX},
};

// split up a section into multiple sections that may be of different or variable types and sizes.
// Make sure the bytes add up to the size of the original section (this is checked at runtime).
//
map<string,vector<subsection_info>> subsection_manual_decode = {
    {"Personal_51",   {{CHAR}, {ZERO,3}}},           // 4 = 1+3
    {"Personal_52",   {{BINARY}, {BULK,3}}},         // 4 = 1+3
    {"Ship_x",        {{BULK,16, 10}, {ZERO,956}}}, // 1116 = 16*10+956
    {"Ship_x_0",      {{SHORT,2, 6}, {uFLOAT}}},        // 16 = 6*2 + 4
    {"Ship_x_1",      {{uFLOAT}, {HEX,4, 3}}},         // 16 = 4 + 4*3
    {"Ship_x_2_6",    {{BINARY}, {BULK,1}}},           // 2 = 1+1
    {"Ship_x_4",      {{SHORT,2,4}, {INT,4,1}, {ZERO,0,1}, {SHORT,2,2}}},   // 16 = 2*4+4+0+2*2
    {"f_x",           {{BULK,2}, {ZERO,98}, {BULK,2}, {ZERO, 1014}}},   // 2+98+2+1014 = 1116
    {"City_x_3",      {{CHAR,1,3}, {BULK,1}}},       // 4 = 3+1. Calling it BULK to match perl. Nonstandard BULK size.
    {"CityInfo_x",    {{BULK, 36}, {BULK, 48}, {BULK, 28}, {BULK, 32}, {BULK,4}}}, // 148 = 36+48+28+32+4
    {"CityInfo_x_0",  {{BULK}, {INT,4,4}, {BULK,4,3}, {INT}}},    //  36 = 4*(1+4+3+1)
    {"CityInfo_x_1",  {{INT}, {BULK}, {INT,4,5}, {SHORT,2,10}}},               //  48 = 4*(1+1+5)+2*10;
    {"Log_x",         {{LCHAR,1,8}, {INT,4,3}, {uFLOAT,4,2}}}, // 28 = 8+4*3+4*2
    {"TreasureMap_x_68", {{BULK,1}, {BINARY,1}, {BULK,1}, {BINARY,1}}}, // 4 = 1+1+1=1
    {"Villain_x",     {{SHORT,2,10}, {INT,4}, {SHORT,2,6}}}, // 36 = 2*10 + 4 + 2*6
    {"CityLoc_x",     {{mFLOAT,4,2}, {HEX,4,2}}}, // 16 = 4*2 + 4*2
    {"t",             {{BULK,8,1}, {BULK,16,7}}},
    {"Top10_x",       {{INT,4,2}, {SHORT,2,10}}}, // 28 = 4*2 + 2*10
    {"Top10_x_1",     {{BINARY}, {ZERO,3}}},
};

// The zero length zero string for Ship_x_4_5 happened because two adjacent shorts were switched
// for an INT and a ZERO. This manuever is required to avoid renumbering (Ship_x_4_7 numbering remained unchanged)
// This would almost be the same as adding these subsection_simple_decodes:
// {"Ship_x_4",      SHORT},
// {"Ship_x_4_5",    INT},
// {"Ship_x_4_6",    ZERO},
// but that isn't allowed. The problem is that ship_x_4_5 would be starting as a SHORT and moving to INT,
// which means growing from 2 bytes to 4. That's not OK; it means the subsection_decode would be going outside of the subsection.


string find_file(string dir, string file, string suffix) {
    // finds a file that is to be packed or unpacked
    // returns the full pathname of the file.
    string game = file;
    game = regex_replace(game, regex("\\." + pst), "");
    game = regex_replace(game, regex("\\." +  pg), "");
    
    const string possible_files[] = {
        game + "." + suffix,
        dir + "/" + game + "." + suffix,
    };
    for (auto afile : possible_files) {
        ifstream f(afile.c_str());
        if (f.good()) {
            return afile;
        }
    }
    cout << "Could not find file " << file << "\n";
    cout << "Looked for: \n";
    for (auto afile : possible_files) {
        cout << "  '" << afile << "'\n";
    }
    exit(1);
}

void unpack_section (section section, ifstream & in, ofstream & out, int offset=0);

void unpack_pg_to_pst(string pg_file, string pst_file) {
    ifstream pg_in = ifstream(pg_file, ios::binary);
    if (! pg_in.is_open()) {
        cerr << "Failed to read from " << pg_file << "\n";
        exit(1);
    }
    cout << "Unpacking " << pg_file << "\n";
    
    ofstream pst_out = ofstream(pst_file);
    if (! pst_out.is_open()) {
        cerr << "Failed to write to " << pst_file << "\n";
        pg_in.close();
        exit(1);
    }
    cout << "Writing " << pst_file << "\n\n";
    
    for (auto section : section_vector) {
        pst_out << "## " << section.name << " starts at byte " << pg_in.tellg() << "\n";
        unpack_section(section, pg_in, pst_out);
    }
    
    pg_in.close();
    pst_out.close();
}

string read_hex(ifstream & in) { // Read 4 bytes from in and report in hex
    char b[4];
    in.read((char*)&b, sizeof(b));
    string res;
    char buffer[255];
    for (int i=3;i>=0;i--) {
        sprintf(buffer, "%02X", (unsigned char)b[i]);
        res += buffer;
        if (i>0) res += '.';
    }
    return res;
}

string read_bulk_hex(ifstream & in, int bytecount) { // Read bytecount bytes from in and report in one big bulk hex
    char b[bytecount];
    in.read((char*)&b, bytecount);
    string res;
    char buffer[20];
    for (int i=0;i<bytecount;i++) {
        sprintf(buffer, "%02X", (unsigned char)b[i]);
        res += buffer;
    }
    return str_tolower(res);
}

string read_zeros(ifstream & in, int bytecount) { // Read bytecount bytes from in that should all be zero.
    char b[bytecount];
    in.read((char*)&b, bytecount);
    for (int i=0;i<bytecount;i++) {
        if (b[i]!= 0) {
            throw logic_error("Found non-zeros");
        }
    }
    return "zero_string";
}


string read_binary(ifstream & in) { // Read 1 byte from in and report as binary string
    char b;
    in.read((char*)&b, sizeof(b));
    std::bitset<8> asbits(b);
    return asbits.to_string();
}

string read_char(ifstream & in) { // Read 1 byte from in and report as an int string
    char b;
    in.read((char*)&b, sizeof(b));
    return to_string(b);
}

string read_short(ifstream &in) { // Read 2 bytes as a signed int and return as string
    char b[2];
    in.read((char*)&b, sizeof(b));
    int B = int((unsigned char)(b[0]) | (char)(b[1]) << 8);
    return to_string(B);
}

int read_int(ifstream & in) { // Read 4 bytes from in (little endian) and convert to integer
    char b[4];
    in.read((char*)&b, sizeof(b));
    int B = (int)((unsigned char)(b[0]) |
                                    (unsigned char)(b[1]) << 8 |
                                    (unsigned char)(b[2]) << 16 |
                                    (char)(b[3]) << 24    );
    return B;
}


string read_uFloat(ifstream &in) {
    auto raw = read_int(in);
    return to_string(double(raw)/1000000);
}


string read_mFloat(ifstream &in) {
    auto raw = read_int(in);
    string retstring = to_string(double(raw)/1000);
    // backward compatibility to match perl:
    retstring = regex_replace(retstring, regex("000$"), "");
    if (retstring == "0.000") { retstring = "0"; }
    return retstring;
}

string read_world_map(ifstream &in, int bytecount, translation_type m, string line_code, vector<feature> & features) {
    unsigned char b[600];
    in.read((char*)&b, bytecount);
    
    vector<bitset<4> > bs(bytecount/4+1, 0);
    
    // The bytes read have values 00, 09, or FF at each byte, except for anomolies.
    // 00 represents sea, FF is land. 09 is for the boundary.
    // To make the output more readable and editable, we compress the land/sea down to single bits and print that a hex
    // and then add lines afterwards to account for anomolies.
  
    for (int i=0; i<bytecount; i++) {
        auto j = i/4;
        auto k = i % 4;
        if (m==CMAP) {
            if (b[i] > 4)  { bs.at(j)[3-k] = 1; }
            if (b[i]!=0 && b[i]!= 9) {
                // anomoly
                features.push_back({ line_code + "_" + to_string(i), b[i]});
            }
        } else {
            if (b[i] != 0) {
                bs.at(j)[3-k] = 1;
                if (b[i] != (unsigned char)(-1)) {
                    // anomoly
                    features.push_back({ line_code + "_" + to_string(i), b[i]});
                }
            }
        }
    }
    stringstream ss;
    for (int j=0; j<bytecount/4+1; j++) {
        ss << std::noshowbase << std::hex << bs[j].to_ulong();
    }
    if (m==SMAP) { return ""; }
    return ss.str();
    
}
void unpack_section (section mysection, ifstream & in, ofstream & out, int offset) {
    if (mysection.name == "Log") { out << "# Ship's Log\n"; } // hack to match perl.
    
    vector<feature> features;
    
    // section: Ship_0_0
    for (int c=offset; c<mysection.count+offset;c++) {
        string subsection = mysection.name + "_" + to_string(c);
        // subsection (could be a line_code)           Ship_0_0_2
        string subsection_x = regex_replace(subsection, regex(R"(^([^_]+)_\d+)"), "$1_x");
        // subsection_x (could be a generic line_code) Ship_x_0_2
        
        // The subsection or subsection_x have higher priority in translating this line
        // than the parent section directive.
        
        if (subsection_simple_decode.count(subsection) || subsection_simple_decode.count(subsection_x)) {
            translation_type submeth = subsection_simple_decode.count(subsection) ?
            subsection_simple_decode.at(subsection) : subsection_simple_decode.at(subsection_x);
            
            if (submeth != mysection.method) { // Avoids infinite loop.
                // Take the the section.byte_count and divide
                // it equally to set up the subsections.
                int count = 1;
                if (size_for_method.at(submeth)>0) {
                    count = mysection.bytes_per_line/size_for_method.at(submeth);
                    
                    if (mysection.bytes_per_line % size_for_method.at(submeth) != 0) {
                        cerr << "Error decoding line " << subsection << " byte counts are not divisible\n";
                        out.close();
                        abort();
                    }
                }
                int suboffset = 0;
                
                if (count ==1) {
                    // If there is only one count for the subsection, then we aren't really splitting it up;
                    // we are changing the translation method of this particular line.
                    // So, remove the numeric ending and call it again with the corrected method.
                    // But note the if above to avoid an infinite loop.
                    subsection = mysection.name;
                    suboffset = c;
                }
                
                struct::section sub = {subsection,count, size_for_method.at(submeth) , submeth };
                unpack_section(sub, in, out, suboffset);
                continue;
            }
        }
        
        if (subsection_manual_decode.count(subsection) || subsection_manual_decode.count(subsection_x)) {
            
            vector<subsection_info> info = subsection_manual_decode.count(subsection) ?
            subsection_manual_decode.at(subsection) : subsection_manual_decode.at(subsection_x);
            
            int suboffset = 0;
            int byte_count_check = 0;
            for (auto subinfo : info) {
                translation_type submeth = subinfo.method;
                struct::section sub = {subsection,subinfo.multiplier, subinfo.byte_count , submeth };
                // cerr << subsection << "," << subinfo.multiplier  << "," << subinfo.byte_count << "\n";
                unpack_section(sub, in, out, suboffset);
                suboffset += subinfo.multiplier;
                byte_count_check += subinfo.multiplier*subinfo.byte_count;
            }
            if (byte_count_check != mysection.bytes_per_line) {
                cerr << "Error decoding line " << subsection << " subsections don't add up: " << byte_count_check << " != " << mysection.bytes_per_line << "\n";
                out.close();
                abort();
                // It will probably crash later anyway.
            }
            continue;
        }
        
        
        //// NOTE a big break here. The code above is for splitting a section, the code below is for reading a line.
        
        auto method = mysection.method;
        auto bytes_per_line = mysection.bytes_per_line;
        
        string value;
        char buffer[255] = "";
        int size_of_string;
        
        switch (method) {
            case TEXT0 : // Stores a length in chars, followed by the text, possibly followed by 0 padding.
                size_of_string = read_int(in);
                if (size_of_string > 100) { out.close(); abort(); }
                in.read((char *)& buffer, size_of_string);
                value = buffer;
                break;
            case TEXT8 : //size_of_string = read_int(in);
                size_of_string = read_int(in);
                if (size_of_string > 100) { out.close(); abort(); }
                in.read((char *)& buffer, size_of_string);
                value = buffer;
                in.read(buffer, 8); // Padding
                break;
            case INT :
                value = to_string(read_int(in));
                break;
            case ZERO :
                try {
                    value = read_zeros(in, bytes_per_line);
                } catch(logic_error) {
                    out.close();
                    abort();
                }
                break;
            case BULK :
                value = read_bulk_hex(in, bytes_per_line);
                break;
            case HEX :
                value = read_hex(in);
                break;
            case FMAP :
            case SMAP :
            case CMAP :
                value = read_world_map(in,bytes_per_line,method, subsection, features);
                break;
            case BINARY :
                value = read_binary(in);
                break;
            case SHORT :
                value = read_short(in);
                break;
            case mFLOAT :
                value = read_mFloat(in);
                break;
            case uFLOAT :
                value = read_uFloat(in);
                break;
            case CHAR :
                value = read_char(in);
                break;
            case LCHAR :
                value = read_char(in);
                break;
            default:
                break;
        }
        
        string translation = full_translate(subsection, value);
        string comment = full_comment(subsection, value);
        
        if (subsection == start_of_fixed_length_section) {
            store_startingyear(in);
        }
        
        // Matching bugs in perl script
        // -1 -> "4294967295";
        if (method==INT && stoi(value) < 0) {
            value = to_string((unsigned int)(stoi(value)));
        }
        
        if (is_world_map(method)) {
            subsection += "_293";
        }
        
        out << subsection << "   : " << char_for_method.at(method) << to_string(bytes_per_line);
        out << "   :   " << value << "   :   " << comment << " " << translation << "\n";
        
    }
    if (is_world_map(mysection.method)) {
        for (auto f : features) {
            // Yes, this is the same as above. I need to refactor.
            string translation = full_translate(f.line_code, to_string(f.v));
            string comment = full_comment(f.line_code, to_string(f.v));
            char buffer[3];
            sprintf(buffer, "%02x", f.v);
            out << f.line_code << "   : F1   :  " << buffer << "  :  " << comment << " " << translation << "\n";
        }
    }
}



int starting_year;
void store_startingyear(ifstream & in) {
    // We're going to jump way forward in the file to read the start year, and then put it into a persistent
    // variable, for use in decoding DateStamps. Then jump right back.
    constexpr int jump_dist = 887272;
    in.seekg(jump_dist, ios_base::cur);
    starting_year = read_int(in);
    in.seekg(-jump_dist-4, ios_base::cur);
    
    
}

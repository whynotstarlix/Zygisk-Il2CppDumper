#ifndef ZYGISK_IL2CPPDUMPER_GAME_H
#define ZYGISK_IL2CPPDUMPER_GAME_H

inline const char *GetGamePackageName() {
    return "com.axlebolt.standoff2";
}

#define GamePackageName GetGamePackageName()

#endif //ZYGISK_IL2CPPDUMPER_GAME_H

#ifndef KSWORD_GUI_KIMAGE_H
#define KSWORD_GUI_KIMAGE_H

// Keep FLTK deprecated aliases disabled for newer FLTK headers and strict builds.
#define FL_NO_DEPRECATED

#include <memory>
#include <string>

class Fl_Image;
class Fl_Shared_Image;
struct KImageResourceCacheEntry;

// KImageResource is a small value wrapper around cached FLTK shared images.
class KImageResource {
public:
    // Creates an empty resource; input is none and the resource has no error.
    KImageResource();

    // Loads an image from a file path through the shared cache.
    explicit KImageResource(const std::string& path);

    // Copies share the same cache entry; input is another resource and no reload occurs.
    KImageResource(const KImageResource& other) = default;

    // Moves transfer the cache entry pointer; input is another resource and no reload occurs.
    KImageResource(KImageResource&& other) noexcept = default;

    // Assigns another resource entry to this object; returns this resource.
    KImageResource& operator=(const KImageResource& other) = default;

    // Move-assigns another resource entry to this object; returns this resource.
    KImageResource& operator=(KImageResource&& other) noexcept = default;

    // Releases this handle's cache entry reference; the global cache is not cleared.
    void reset();

    // Creates a resource from a file path, reusing a cached success or failure entry.
    static KImageResource Load(const std::string& path);

    // Creates a resource from a file path using legacy factory naming; returns the loaded resource.
    static KImageResource FromFile(const std::string& path);

    // Creates a resource from a file path using snake_case factory naming; returns the loaded resource.
    static KImageResource from_file(const std::string& path);

    // Loads a file into this resource and returns true when a drawable image is available.
    bool loadFromFile(const std::string& path);

    // Loads a file using snake_case compatibility naming and returns true on success.
    bool load_from_file(const std::string& path);

    // Returns the file path associated with this resource, or an empty string when unset.
    const std::string& path() const;

    // Returns the drawable image width in FLTK units, or 0 when invalid.
    int width() const;

    // Returns the drawable image height in FLTK units, or 0 when invalid.
    int height() const;

    // Returns true when loading succeeded and image() can be drawn.
    bool valid() const;

    // Returns the last load error, or an empty string when valid or never loaded.
    const std::string& error() const;

    // Returns the cached FLTK image pointer; caller must not delete or release it.
    Fl_Image* image() const;

    // Returns the cached shared image pointer; caller must not delete or release it.
    Fl_Shared_Image* sharedImage() const;

    // Returns true when the exact path has a cached success or failure entry.
    static bool cached(const std::string& path);

    // Removes one exact path from the cache; live KImageResource handles remain valid.
    static void removeFromCache(const std::string& path);

    // Clears all cached entries; live KImageResource handles keep their images alive.
    static void clearCache();

private:
    std::shared_ptr<KImageResourceCacheEntry> entry_;
};

#endif // KSWORD_GUI_KIMAGE_H

#ifndef KSWORD_HELPER_KGEOMETRY_HEAD_FILE
#define KSWORD_HELPER_KGEOMETRY_HEAD_FILE

// KPoint stores GUI-independent integer coordinates.
class KPoint {
public:
    // Constructors initialize coordinates to zero or caller-provided values.
    KPoint();
    KPoint(int x, int y);

    // Accessors and mutators read or write coordinates and return no heap data.
    int x() const;
    int y() const;
    void setX(int x);
    void setY(int y);

    // translated returns a moved copy and leaves this point unchanged.
    KPoint translated(int dx, int dy) const;

    // Equality operators compare both coordinates.
    bool operator==(const KPoint& other) const;
    bool operator!=(const KPoint& other) const;

private:
    // m_x and m_y are plain integer coordinates.
    int m_x;
    int m_y;
};

// KSize stores GUI-independent integer dimensions.
class KSize {
public:
    // Constructors initialize dimensions to zero or caller-provided values.
    KSize();
    KSize(int width, int height);

    // Accessors and mutators read or write dimensions.
    int width() const;
    int height() const;
    void setWidth(int width);
    void setHeight(int height);

    // isEmpty treats non-positive width or height as empty.
    bool isEmpty() const;

    // isValid requires both dimensions to be zero or positive.
    bool isValid() const;

    // expandedTo and boundedTo compare each dimension independently.
    KSize expandedTo(const KSize& other) const;
    KSize boundedTo(const KSize& other) const;

    // Equality operators compare width and height.
    bool operator==(const KSize& other) const;
    bool operator!=(const KSize& other) const;

private:
    // m_width and m_height are plain integer dimensions.
    int m_width;
    int m_height;
};

// KRect stores a GUI-independent rectangle as top-left plus size. The right and
// bottom edges use exclusive coordinates, matching many layout calculations.
class KRect {
public:
    // Constructors initialize an empty rectangle or caller-provided geometry.
    KRect();
    KRect(int x, int y, int width, int height);
    KRect(const KPoint& topLeft, const KSize& size);

    // Accessors expose top-left, size, and exclusive edge coordinates.
    int x() const;
    int y() const;
    int width() const;
    int height() const;
    int left() const;
    int top() const;
    int right() const;
    int bottom() const;
    KPoint topLeft() const;
    KSize size() const;

    // Mutators update individual fields or grouped values and return no value.
    void setX(int x);
    void setY(int y);
    void setWidth(int width);
    void setHeight(int height);
    void setTopLeft(const KPoint& point);
    void setSize(const KSize& size);

    // isEmpty treats non-positive width or height as empty.
    bool isEmpty() const;

    // contains returns true when point lies inside the exclusive rectangle bounds.
    bool contains(const KPoint& point) const;
    bool contains(int x, int y) const;

    // translated returns a moved copy and leaves this rectangle unchanged.
    KRect translated(int dx, int dy) const;

    // intersects, intersected, and united provide basic rectangle set operations.
    bool intersects(const KRect& other) const;
    KRect intersected(const KRect& other) const;
    KRect united(const KRect& other) const;

    // Equality operators compare all four stored fields.
    bool operator==(const KRect& other) const;
    bool operator!=(const KRect& other) const;

private:
    // The rectangle stores top-left plus size to match constructor inputs.
    int m_x;
    int m_y;
    int m_width;
    int m_height;
};

#endif

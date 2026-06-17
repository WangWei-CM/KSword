#include "KGeometry.h"

#include <algorithm>

KPoint::KPoint()
    : m_x(0), m_y(0) {
}

KPoint::KPoint(int x, int y)
    : m_x(x), m_y(y) {
}

int KPoint::x() const {
    return m_x;
}

int KPoint::y() const {
    return m_y;
}

void KPoint::setX(int x) {
    m_x = x;
}

void KPoint::setY(int y) {
    m_y = y;
}

KPoint KPoint::translated(int dx, int dy) const {
    return KPoint(m_x + dx, m_y + dy);
}

bool KPoint::operator==(const KPoint& other) const {
    return m_x == other.m_x && m_y == other.m_y;
}

bool KPoint::operator!=(const KPoint& other) const {
    return !(*this == other);
}

KSize::KSize()
    : m_width(0), m_height(0) {
}

KSize::KSize(int width, int height)
    : m_width(width), m_height(height) {
}

int KSize::width() const {
    return m_width;
}

int KSize::height() const {
    return m_height;
}

void KSize::setWidth(int width) {
    m_width = width;
}

void KSize::setHeight(int height) {
    m_height = height;
}

bool KSize::isEmpty() const {
    return m_width <= 0 || m_height <= 0;
}

bool KSize::isValid() const {
    return m_width >= 0 && m_height >= 0;
}

KSize KSize::expandedTo(const KSize& other) const {
    return KSize(std::max(m_width, other.m_width), std::max(m_height, other.m_height));
}

KSize KSize::boundedTo(const KSize& other) const {
    return KSize(std::min(m_width, other.m_width), std::min(m_height, other.m_height));
}

bool KSize::operator==(const KSize& other) const {
    return m_width == other.m_width && m_height == other.m_height;
}

bool KSize::operator!=(const KSize& other) const {
    return !(*this == other);
}

KRect::KRect()
    : m_x(0), m_y(0), m_width(0), m_height(0) {
}

KRect::KRect(int x, int y, int width, int height)
    : m_x(x), m_y(y), m_width(width), m_height(height) {
}

KRect::KRect(const KPoint& topLeft, const KSize& size)
    : m_x(topLeft.x()), m_y(topLeft.y()), m_width(size.width()), m_height(size.height()) {
}

int KRect::x() const {
    return m_x;
}

int KRect::y() const {
    return m_y;
}

int KRect::width() const {
    return m_width;
}

int KRect::height() const {
    return m_height;
}

int KRect::left() const {
    return m_x;
}

int KRect::top() const {
    return m_y;
}

int KRect::right() const {
    return m_x + m_width;
}

int KRect::bottom() const {
    return m_y + m_height;
}

KPoint KRect::topLeft() const {
    return KPoint(m_x, m_y);
}

KSize KRect::size() const {
    return KSize(m_width, m_height);
}

void KRect::setX(int x) {
    m_x = x;
}

void KRect::setY(int y) {
    m_y = y;
}

void KRect::setWidth(int width) {
    m_width = width;
}

void KRect::setHeight(int height) {
    m_height = height;
}

void KRect::setTopLeft(const KPoint& point) {
    m_x = point.x();
    m_y = point.y();
}

void KRect::setSize(const KSize& size) {
    m_width = size.width();
    m_height = size.height();
}

bool KRect::isEmpty() const {
    return m_width <= 0 || m_height <= 0;
}

bool KRect::contains(const KPoint& point) const {
    return contains(point.x(), point.y());
}

bool KRect::contains(int x, int y) const {
    return !isEmpty() && x >= left() && x < right() && y >= top() && y < bottom();
}

KRect KRect::translated(int dx, int dy) const {
    return KRect(m_x + dx, m_y + dy, m_width, m_height);
}

bool KRect::intersects(const KRect& other) const {
    if (isEmpty() || other.isEmpty()) {
        return false;
    }
    return left() < other.right() && other.left() < right() && top() < other.bottom() && other.top() < bottom();
}

KRect KRect::intersected(const KRect& other) const {
    if (!intersects(other)) {
        return KRect();
    }

    const int newLeft = std::max(left(), other.left());
    const int newTop = std::max(top(), other.top());
    const int newRight = std::min(right(), other.right());
    const int newBottom = std::min(bottom(), other.bottom());
    return KRect(newLeft, newTop, newRight - newLeft, newBottom - newTop);
}

KRect KRect::united(const KRect& other) const {
    if (isEmpty()) {
        return other;
    }
    if (other.isEmpty()) {
        return *this;
    }

    const int newLeft = std::min(left(), other.left());
    const int newTop = std::min(top(), other.top());
    const int newRight = std::max(right(), other.right());
    const int newBottom = std::max(bottom(), other.bottom());
    return KRect(newLeft, newTop, newRight - newLeft, newBottom - newTop);
}

bool KRect::operator==(const KRect& other) const {
    return m_x == other.m_x && m_y == other.m_y && m_width == other.m_width && m_height == other.m_height;
}

bool KRect::operator!=(const KRect& other) const {
    return !(*this == other);
}

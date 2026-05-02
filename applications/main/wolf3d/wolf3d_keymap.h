/* wolf3d_keymap.h — Drop-in-Replacement für std::unordered_map.
 *
 * Wolf4SDL benutzt std::unordered_map nur für zwei kleine Lookup-Tables
 * (Keyboard-Status und ScanNames). Eine eigene Linear-Search-Map mit
 * fester Kapazität reicht funktional und vermeidet libstdc++-Linkage,
 * was in einer FAP-Welt 5 unauflösbare STL-Symbole eliminiert
 * (operator new/delete, __throw_bad_alloc, _Prime_rehash_policy).
 *
 * API kompatibel zu dem Subset, das Wolf4SDL nutzt:
 *   m[k] = v;          // insert / overwrite
 *   v = m[k];          // lookup (insert with V{} if missing)
 *   m.clear();
 *   m.find(k); m.end(); it->second;
 *   for(auto& kv : m) ...
 */

#ifndef WOLF3D_KEYMAP_H
#define WOLF3D_KEYMAP_H

#ifdef __cplusplus

template<typename K, typename V>
class WolfMap {
public:
    struct value_type {
        K first;
        V second;
    };

private:
    static constexpr int CAP = 256;
    value_type data[CAP];
    bool       used[CAP];
    int        max_idx;

public:
    WolfMap() : max_idx(0) {
        for(int i = 0; i < CAP; i++) used[i] = false;
    }

    V& operator[](K k) {
        for(int i = 0; i < max_idx; i++)
            if(used[i] && data[i].first == k) return data[i].second;
        for(int i = 0; i < CAP; i++) {
            if(!used[i]) {
                data[i].first  = k;
                data[i].second = V{};
                used[i] = true;
                if(i >= max_idx) max_idx = i + 1;
                return data[i].second;
            }
        }
        static V dummy{};
        return dummy;
    }

    void clear() {
        for(int i = 0; i < max_idx; i++) used[i] = false;
        max_idx = 0;
    }

    struct iterator {
        WolfMap* m;
        int      idx;

        iterator& operator++() {
            do { ++idx; } while(idx < m->max_idx && !m->used[idx]);
            return *this;
        }
        bool operator==(const iterator& o) const { return idx == o.idx; }
        bool operator!=(const iterator& o) const { return idx != o.idx; }
        value_type& operator*()  { return m->data[idx]; }
        value_type* operator->() { return &m->data[idx]; }
    };

    iterator begin() {
        iterator it{this, 0};
        while(it.idx < max_idx && !used[it.idx]) ++it.idx;
        return it;
    }
    iterator end() { return iterator{this, max_idx}; }
    iterator find(K k) {
        for(int i = 0; i < max_idx; i++)
            if(used[i] && data[i].first == k) return iterator{this, i};
        return end();
    }
};

#endif /* __cplusplus */
#endif /* WOLF3D_KEYMAP_H */

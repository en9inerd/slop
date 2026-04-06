package server

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"sync"
	"time"
)

type connPool struct {
	mu    sync.Mutex
	conns []*conn
	max   int
}

func newPool(max int) *connPool {
	return &connPool{max: max}
}

func (p *connPool) Get(ctx context.Context) (*conn, error) {
	p.mu.Lock()
	defer p.mu.Unlock()

	if len(p.conns) > 0 {
		c := p.conns[len(p.conns)-1]
		p.conns = p.conns[:len(p.conns)-1]
		return c, nil
	}

	if p.max > 0 && len(p.conns) >= p.max {
		return nil, fmt.Errorf("pool exhausted")
	}

	return dial(ctx)
}

func (p *connPool) Put(c *conn) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.conns = append(p.conns, c)
}

type conn struct {
	alive bool
	t     time.Time
}

func dial(ctx context.Context) (*conn, error) {
	select {
	case <-ctx.Done():
		return nil, ctx.Err()
	default:
		return &conn{alive: true, t: time.Now()}, nil
	}
}

type handler struct {
	pool *connPool
}

func (h *handler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	c, err := h.pool.Get(r.Context())
	if err != nil {
		http.Error(w, err.Error(), 503)
		return
	}
	defer h.pool.Put(c)

	var req struct {
		Query string `json:"q"`
	}
	json.NewDecoder(r.Body).Decode(&req)

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{
		"ok":   true,
		"took": time.Since(c.t).Milliseconds(),
	})
}

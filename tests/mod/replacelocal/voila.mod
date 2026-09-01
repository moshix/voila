module example.com/app

require example.com/lib v9.9.9

// The required version does not exist anywhere; `replace` decides where the
// module comes from, and a local target is never hashed against voila.sum.
replace example.com/lib => ./local
